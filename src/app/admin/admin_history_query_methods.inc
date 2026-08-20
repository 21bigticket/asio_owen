    void start_list() {
        if (ctx_.method != "GET") {
            method_not_allowed(ctx_, "GET");
            complete();
            return;
        }
        int64_t before = 0;
        size_t limit = services_.config_sync.history.history_page_size;
        if (auto value = query_value(ctx_.path, "before")) {
            auto parsed = config_history::parse_int64(*value);
            if (!parsed || *parsed <= 0) {
                bad_request("invalid before cursor");
                return;
            }
            before = *parsed;
        }
        if (auto value = query_value(ctx_.path, "limit")) {
            auto parsed = config_history::parse_int64(*value);
            if (!parsed || *parsed <= 0 ||
                static_cast<uint64_t>(*parsed) >
                    services_.config_sync.history.history_page_size_max) {
                bad_request("invalid history limit");
                return;
            }
            limit = static_cast<size_t>(*parsed);
        }
        auto self = shared_from_this();
        run_command({"EVAL", config_history::list_script(), "3",
                     std::string(config_admin::kVersionKey),
                     std::string(config_history::kIndexKey),
                     std::string(config_history::kMetaKey),
                     std::to_string(before), std::to_string(limit),
                     std::string(config_history::kSnapshotPrefix)},
            [self](RedisPool::Reply reply) {
                if (!reply.ok) {
                    self->redis_failed(reply.error);
                    return;
                }
                if (reply.type != "array" || reply.elements.empty()) {
                    self->inconsistent("history list returned malformed data");
                    return;
                }
                auto current = config_history::parse_int64(reply.elements.front());
                if (!current || *current < 0) {
                    self->inconsistent("history version is not numeric");
                    return;
                }
                std::vector<std::string> rows(
                    reply.elements.begin() + 1, reply.elements.end());
                if (rows.size() % 2 != 0) {
                    self->inconsistent("history list returned malformed rows");
                    return;
                }
                self->ctx_.status_code = 200;
                self->ctx_.response_body = resp_ok(
                    config_history::history_list_json(rows, *current));
                self->complete();
            });
    }

    void start_path() {
        if (ctx_.method != "GET") {
            method_not_allowed(ctx_, "GET");
            complete();
            return;
        }
        static constexpr std::string_view prefix =
            "/api/admin/config/history/";
        const std::string path = path_only(ctx_.path);
        if (path.rfind(prefix, 0) != 0) {
            not_found();
            return;
        }
        std::string suffix = path.substr(prefix.size());
        bool diff = false;
        static constexpr std::string_view diff_suffix = "/diff";
        if (suffix.size() > diff_suffix.size() &&
            suffix.compare(suffix.size() - diff_suffix.size(),
                diff_suffix.size(), diff_suffix) == 0) {
            diff = true;
            suffix.resize(suffix.size() - diff_suffix.size());
        }
        auto version = config_history::parse_int64(suffix);
        if (!version || *version <= 0) {
            not_found();
            return;
        }
        if (diff) start_diff(*version);
        else start_detail(*version);
    }

    void start_detail(int64_t version) {
        auto self = shared_from_this();
        read_detail(version,
            [self, version](
                std::optional<config_history::SnapshotRecord> record) {
                if (!record) {
                    self->not_found();
                    return;
                }
                if (!valid_record_hash(*record)) {
                    self->inconsistent("history snapshot hash mismatch");
                    return;
                }
                self->ctx_.status_code = 200;
                self->ctx_.response_body = resp_ok(
                    config_history::detail_json(version, *record));
                self->complete();
            });
    }

    void start_diff(int64_t from_version) {
        auto requested_to = query_value(ctx_.path, "to");
        std::optional<int64_t> to_version;
        if (requested_to) {
            to_version = config_history::parse_int64(*requested_to);
            if (!to_version || *to_version <= 0) {
                bad_request("invalid diff target version");
                return;
            }
        }
        auto self = shared_from_this();
        read_detail(from_version,
            [self, from_version, to_version](
                std::optional<config_history::SnapshotRecord> from) mutable {
                if (!from) {
                    self->not_found();
                    return;
                }
                if (!valid_record_hash(*from)) {
                    self->inconsistent("history source hash mismatch");
                    return;
                }
                const int64_t target = to_version.value_or(from->current_version);
                if (target == from_version) {
                    auto result = config_history::diff_json(
                        from_version, *from, target, *from,
                        self->services_.config_sync.history.max_diff_response_bytes);
                    self->ctx_.status_code = 200;
                    self->ctx_.response_body = resp_ok(result.value_or(
                        "{\"from\":" + std::to_string(from_version) +
                        ",\"to\":" + std::to_string(target) +
                        ",\"changes\":[]}"));
                    self->complete();
                    return;
                }
                auto saved_from = std::make_shared<config_history::SnapshotRecord>(
                    std::move(*from));
                self->read_detail(target,
                    [self, from_version, target, saved_from](
                        std::optional<config_history::SnapshotRecord> to) {
                        if (!to) {
                            self->not_found();
                            return;
                        }
                        if (!valid_record_hash(*to)) {
                            self->inconsistent("history target hash mismatch");
                            return;
                        }
                        auto result = config_history::diff_json(
                            from_version, *saved_from, target, *to,
                            self->services_.config_sync.history.max_diff_response_bytes);
                        if (!result) {
                            self->ctx_.status_code = 413;
                            self->ctx_.response_body = resp_err(
                                413, "diff response exceeds configured limit");
                            self->complete();
                            return;
                        }
                        self->ctx_.status_code = 200;
                        self->ctx_.response_body = resp_ok(*result);
                        self->complete();
                    });
            });
    }
