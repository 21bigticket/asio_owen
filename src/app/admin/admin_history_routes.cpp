// History route entry points are implemented alongside the operation state
// machine in admin_history_operation.cpp.  This translation unit is kept as
// the stable route-module boundary so future route registration changes do
// not grow the operation implementation again.

#include "../routes.hpp"
