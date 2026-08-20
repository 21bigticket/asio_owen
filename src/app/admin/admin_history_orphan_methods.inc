    void start_orphan_resolution() {
        if (ctx_.method != "POST") {
            method_not_allowed(ctx_, "POST");
            complete();
            return;
        }
        auto parsed = config_admin::parse_orphan_resolution_request(ctx_.body);
        if (!parsed.ok) {
            bad_request(parsed.error);
            return;
        }
        if (parsed.request.reason.size() >
            services_.config_sync.history.max_reason_bytes) {
            bad_request("reason exceeds configured limit");
            return;
        }
        orphan_request_ = std::move(parsed.request);
        auto self = shared_from_this();
        run_command({"EVAL", config_history::orphan_inspect_script(), "5",
                     std::string(config_admin::kVersionKey),
                     std::string(config_history::kIndexKey),
                     std::string(config_history::kMetaKey),
                     config_history::snapshot_key(orphan_request_.target_version),
                     std::string(config_admin::kMachinesKey),
                     std::to_string(orphan_request_.target_version)},
            [self](RedisPool::Reply reply) {
                self->prepare_orphan_resolution(std::move(reply));
            });
    }

    void prepare_orphan_resolution(RedisPool::Reply reply) {
        if (!reply.ok || reply.type != "array" || reply.elements.size() < 6 ||
            (reply.elements.size() - 6) % 2 != 0 ||
            reply.elements.front() == "__error__") {
            inconsistent("orphan state is missing, malformed or changed");
            return;
        }
        auto current = config_history::parse_int64(reply.elements[0]);
        auto index_high = config_history::parse_int64(reply.elements[1]);
        auto machine_high = config_history::parse_int64(reply.elements[2]);
        if (!current || !index_high || !machine_high ||
            *current != orphan_request_.current_version) {
            inconsistent("orphan state changed; inspect again");
            return;
        }
        const bool indexed = reply.elements[3] == "1";
        const std::string meta = reply.elements[4];
        const bool snapshot_exists = reply.elements[5] == "1";
        if ((reply.elements[3] != "0" && !indexed) ||
            (reply.elements[5] != "0" && !snapshot_exists)) {
            inconsistent("orphan inspection returned malformed flags");
            return;
        }
        std::map<std::string, std::string> files;
        for (size_t i = 6; i < reply.elements.size(); i += 2) {
            files[reply.elements[i]] = reply.elements[i + 1];
        }
        const int64_t observed = services_.config_history_service ?
            services_.config_history_service->stats().max_observed_version : 0;
        if (orphan_request_.action == "restore-version") {
            if (!indexed || meta == "__missing__" || !snapshot_exists ||
                files.empty() || *index_high != orphan_request_.target_version ||
                *machine_high > orphan_request_.target_version ||
                observed > orphan_request_.target_version) {
                inconsistent("target is not a complete trusted high-water snapshot");
                return;
            }
            config_history::SnapshotRecord record;
            record.meta_json = meta;
            record.files = files;
            if (!valid_record_hash(record)) {
                inconsistent("orphan snapshot hash does not match metadata");
                return;
            }
            config_history::SnapshotInfo info;
            if (auto error = config_history::validate_snapshot(
                    files, services_.config_sync.history, info)) {
                inconsistent(*error);
                return;
            }
            submit_restore_version(std::move(files), meta);
            return;
        }
        if (orphan_request_.target_version != *current + 1 ||
            *machine_high > *current || observed > *current ||
            (!indexed && meta == "__missing__" && !snapshot_exists)) {
            inconsistent("delete refused: published-version evidence exists or target is not current+1");
            return;
        }
        submit_delete_orphan(std::move(files), meta, indexed, snapshot_exists);
    }
