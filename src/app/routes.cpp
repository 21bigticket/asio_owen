#include "routes.hpp"

#include "public_routes.hpp"

void register_routes(HttpServer& server, AppServices services) {
    server.route("/api/health", api_health);
    server.route("/api/ready", [services](HttpContext& ctx) {
        return api_ready(ctx, services);
    });
    server.route("/api/metrics", [services](HttpContext& ctx) {
        return api_metrics(ctx, services);
    });
    server.route("/api/build", api_build);
    server.route("/admin", handle_admin_page);
    server.route("/admin/login", handle_admin_page);
    server.route("/admin/settings", handle_admin_settings_page);
    server.route("/api/redis", [services](HttpContext& ctx) {
        return api_redis(ctx, services);
    });
    server.route("/api/mysql", [services](HttpContext& ctx) {
        return api_mysql(ctx, services);
    });
    server.route("/api/combo", [services](HttpContext& ctx) {
        return handle_api_combo(ctx, services);
    });
    server.route("/api/admin/login", [services](HttpContext& ctx) {
        return handle_api_admin_login(ctx, services);
    });
    server.route("/api/admin/config", [services](HttpContext& ctx) {
        return handle_api_admin_config(ctx, services);
    });
    server.route("/api/admin/config/machines", [services](HttpContext& ctx) {
        return handle_api_admin_machines(ctx, services);
    });
    server.route("/api/admin/config/history", [services](HttpContext& ctx) {
        return handle_api_admin_history(ctx, services);
    });
    server.route_prefix("/api/admin/config/history/", [services](HttpContext& ctx) {
        return handle_api_admin_history_path(ctx, services);
    });
    server.route("/api/admin/config/rollback", [services](HttpContext& ctx) {
        return handle_api_admin_rollback(ctx, services);
    });
    server.route("/api/admin/config/history/repair-snapshot", [services](HttpContext& ctx) {
        return handle_api_admin_snapshot_repair(ctx, services);
    });
    server.route("/api/admin/config/history/rebuild-mirror", [services](HttpContext& ctx) {
        return handle_api_admin_mirror_rebuild(ctx, services);
    });
    server.route("/api/admin/config/history/migrate", [services](HttpContext& ctx) {
        return handle_api_admin_history_migration(ctx, services);
    });
    server.route("/api/admin/config/history/resolve-orphan", [services](HttpContext& ctx) {
        return handle_api_admin_orphan_resolution(ctx, services);
    });
}
