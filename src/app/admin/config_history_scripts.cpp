#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <openssl/evp.h>

#include "../../http/response.hpp"
#include "../app_config.hpp"
#include "config_history.hpp"

namespace config_history {

std::string list_script() {
    return R"(
local cur = tonumber(redis.call('GET', KEYS[1]) or '')
local before = tonumber(ARGV[1])
local limit = tonumber(ARGV[2])
if cur == nil or before == nil or limit == nil or limit < 1 then return {'__error__', 'invalid'} end
local max_score = '+inf'
if before > 0 then max_score = '(' .. tostring(before) end
local versions = redis.call('ZREVRANGEBYSCORE', KEYS[2], max_score, '-inf', 'LIMIT', 0, limit)
local out = {tostring(cur)}
local metas = {}
if #versions > 0 then metas = redis.call('HMGET', KEYS[3], unpack(versions)) end
for i, version in ipairs(versions) do
  local numeric = tonumber(version)
  local meta = metas[i]
  if numeric ~= nil and numeric <= cur and meta and
     redis.call('EXISTS', ARGV[3] .. version) == 1 then
    table.insert(out, version)
    table.insert(out, meta)
  end
end
return out
)";
}

std::string detail_script() {
    return R"(
local cur = tonumber(redis.call('GET', KEYS[1]) or '')
local version = tonumber(ARGV[1])
if cur == nil or version == nil or version > cur then return {} end
local member = tostring(version)
local meta = redis.call('HGET', KEYS[2], member)
if not meta or not redis.call('ZSCORE', KEYS[3], member) or
   redis.call('EXISTS', KEYS[4]) ~= 1 then return {} end
local files = redis.call('HGETALL', KEYS[4])
if #files == 0 then return {} end
local out = {tostring(cur), meta}
for _, value in ipairs(files) do table.insert(out, value) end
return out
)";
}

std::string save_script() {
    return R"(
if (#ARGV - 6) % 2 ~= 0 then return -2 end
if #ARGV < 8 then return -2 end
local base = tonumber(ARGV[1])
local max_files = tonumber(ARGV[4])
local max_file_bytes = tonumber(ARGV[5])
local max_total_bytes = tonumber(ARGV[6])
if base == nil or max_files == nil or max_file_bytes == nil or max_total_bytes == nil then return -2 end
local file_count = (#ARGV - 6) / 2
if file_count < 1 or file_count > max_files then return -8 end
local total_bytes = 0
for i = 7, #ARGV, 2 do
  local file_bytes = string.len(ARGV[i + 1])
  if file_bytes > max_file_bytes then return -8 end
  total_bytes = total_bytes + file_bytes
  if total_bytes > max_total_bytes then return -8 end
end
local expected = {'string', 'hash', 'list', 'hash', 'hash', 'zset', 'hash', 'hash', 'hash'}
for i = 1, #KEYS do
  local t = redis.call('TYPE', KEYS[i]).ok
  if t ~= expected[i] and t ~= 'none' then return -3 end
end
local cur = tonumber(redis.call('GET', KEYS[1]) or '0')
if cur == nil then return -4 end
if cur ~= base then return -1 end
local top = redis.call('ZREVRANGE', KEYS[6], 0, 0)
if cur > 0 then
  local current_member = tostring(cur)
  if top[1] == nil or tonumber(top[1]) ~= cur or
     not redis.call('HGET', KEYS[5], current_member) or
     redis.call('EXISTS', KEYS[9]) ~= 1 then return -5 end
elseif top[1] ~= nil or redis.call('ZCARD', KEYS[6]) ~= 0 or
   redis.call('HLEN', KEYS[5]) ~= 0 then
  return -5
end
local newv = base + 1
local member = tostring(newv)
if redis.call('EXISTS', KEYS[7]) ~= 0 or
   redis.call('HEXISTS', KEYS[5], member) ~= 0 or
   redis.call('ZSCORE', KEYS[6], member) ~= false then return -6 end
redis.call('DEL', KEYS[8])
redis.call('HSET', KEYS[8], unpack(ARGV, 7, #ARGV))
redis.call('RENAME', KEYS[8], KEYS[7])
redis.call('HSET', KEYS[5], member, ARGV[3])
redis.call('ZADD', KEYS[6], newv, member)
redis.call('DEL', KEYS[4])
redis.call('HSET', KEYS[4], unpack(ARGV, 7, #ARGV))
redis.call('RENAME', KEYS[4], KEYS[2])
local published = redis.call('INCR', KEYS[1])
if published ~= newv then return -7 end
pcall(function()
  redis.call('LPUSH', KEYS[3], ARGV[2])
  redis.call('LTRIM', KEYS[3], 0, 199)
end)
return published
)";
}

std::string rollback_script() {
    return R"(
if (#ARGV - 8) % 2 ~= 0 then return -2 end
if #ARGV < 10 then return -2 end
local base = tonumber(ARGV[1])
local max_files = tonumber(ARGV[4])
local max_file_bytes = tonumber(ARGV[5])
local max_total_bytes = tonumber(ARGV[6])
local source_version = tonumber(ARGV[8])
if base == nil or max_files == nil or max_file_bytes == nil or
   max_total_bytes == nil or source_version == nil then return -2 end
local file_count = (#ARGV - 8) / 2
if file_count < 1 or file_count > max_files then return -8 end
local total_bytes = 0
for i = 9, #ARGV, 2 do
  local file_bytes = string.len(ARGV[i + 1])
  if file_bytes > max_file_bytes then return -8 end
  total_bytes = total_bytes + file_bytes
  if total_bytes > max_total_bytes then return -8 end
end
local expected = {'string', 'hash', 'list', 'hash', 'hash', 'zset', 'hash', 'hash', 'hash', 'hash'}
for i = 1, #KEYS do
  local t = redis.call('TYPE', KEYS[i]).ok
  if t ~= expected[i] and t ~= 'none' then return -3 end
end
local cur = tonumber(redis.call('GET', KEYS[1]) or '0')
if cur == nil then return -4 end
if cur ~= base then return -1 end
local top = redis.call('ZREVRANGE', KEYS[6], 0, 0)
if cur > 0 then
  local current_member = tostring(cur)
  if top[1] == nil or tonumber(top[1]) ~= cur or
     not redis.call('HGET', KEYS[5], current_member) or
     redis.call('EXISTS', KEYS[10]) ~= 1 then return -5 end
elseif top[1] ~= nil or redis.call('ZCARD', KEYS[6]) ~= 0 or
   redis.call('HLEN', KEYS[5]) ~= 0 then
  return -5
end
local source_member = tostring(source_version)
if source_version > cur or redis.call('EXISTS', KEYS[9]) ~= 1 or
   redis.call('HGET', KEYS[5], source_member) ~= ARGV[7] or
   not redis.call('ZSCORE', KEYS[6], source_member) or
   redis.call('HLEN', KEYS[9]) ~= file_count then return -9 end
for i = 9, #ARGV, 2 do
  if redis.call('HGET', KEYS[9], ARGV[i]) ~= ARGV[i + 1] then return -9 end
end
local newv = base + 1
local member = tostring(newv)
if redis.call('EXISTS', KEYS[7]) ~= 0 or
   redis.call('HEXISTS', KEYS[5], member) ~= 0 or
   redis.call('ZSCORE', KEYS[6], member) ~= false then return -6 end
redis.call('DEL', KEYS[8])
redis.call('HSET', KEYS[8], unpack(ARGV, 9, #ARGV))
redis.call('RENAME', KEYS[8], KEYS[7])
redis.call('HSET', KEYS[5], member, ARGV[3])
redis.call('ZADD', KEYS[6], newv, member)
redis.call('DEL', KEYS[4])
redis.call('HSET', KEYS[4], unpack(ARGV, 9, #ARGV))
redis.call('RENAME', KEYS[4], KEYS[2])
local published = redis.call('INCR', KEYS[1])
if published ~= newv then return -7 end
pcall(function()
  redis.call('LPUSH', KEYS[3], ARGV[2])
  redis.call('LTRIM', KEYS[3], 0, 199)
end)
return published
)";
}

std::string snapshot_repair_script() {
    return R"(
if (#ARGV - 6) % 2 ~= 0 then return -2 end
if #ARGV < 8 then return -2 end
local version = tonumber(ARGV[1])
local max_files = tonumber(ARGV[4])
local max_file_bytes = tonumber(ARGV[5])
local max_total_bytes = tonumber(ARGV[6])
if version == nil or max_files == nil or max_file_bytes == nil or max_total_bytes == nil then return -2 end
local file_count = (#ARGV - 6) / 2
if file_count < 1 or file_count > max_files then return -8 end
local total_bytes = 0
for i = 7, #ARGV, 2 do
  local file_bytes = string.len(ARGV[i + 1])
  if file_bytes > max_file_bytes then return -8 end
  total_bytes = total_bytes + file_bytes
  if total_bytes > max_total_bytes then return -8 end
end
local expected = {'string', 'hash', 'hash', 'zset', 'hash', 'hash', 'list'}
for i = 1, #KEYS do
  local t = redis.call('TYPE', KEYS[i]).ok
  if t ~= expected[i] and t ~= 'none' then return -3 end
end
local cur = tonumber(redis.call('GET', KEYS[1]) or '')
if cur == nil then return -4 end
if cur ~= version then return -1 end
local top = redis.call('ZREVRANGE', KEYS[4], 0, 0)
if top[1] ~= nil then
  local high = tonumber(top[1])
  if high == nil or high > cur then return -5 end
end
local member = tostring(version)
if redis.call('EXISTS', KEYS[5]) ~= 0 then return -6 end
if redis.call('HGET', KEYS[3], member) ~= ARGV[2] or
   not redis.call('ZSCORE', KEYS[4], member) or
   redis.call('HLEN', KEYS[2]) ~= file_count then return -9 end
for i = 7, #ARGV, 2 do
  if redis.call('HGET', KEYS[2], ARGV[i]) ~= ARGV[i + 1] then return -9 end
end
redis.call('DEL', KEYS[6])
redis.call('HSET', KEYS[6], unpack(ARGV, 7, #ARGV))
redis.call('RENAME', KEYS[6], KEYS[5])
pcall(function()
  redis.call('LPUSH', KEYS[7], ARGV[3])
  redis.call('LTRIM', KEYS[7], 0, 199)
end)
return version
)";
}

std::string mirror_rebuild_script() {
    return R"(
if (#ARGV - 6) % 2 ~= 0 then return -2 end
if #ARGV < 8 then return -2 end
local version = tonumber(ARGV[1])
local max_files = tonumber(ARGV[4])
local max_file_bytes = tonumber(ARGV[5])
local max_total_bytes = tonumber(ARGV[6])
if version == nil or max_files == nil or max_file_bytes == nil or max_total_bytes == nil then return -2 end
local file_count = (#ARGV - 6) / 2
if file_count < 1 or file_count > max_files then return -8 end
local expected = {'string', 'hash', 'hash', 'zset', 'hash', 'hash', 'list'}
for i = 1, #KEYS do
  local t = redis.call('TYPE', KEYS[i]).ok
  if t ~= expected[i] and t ~= 'none' then return -3 end
end
local cur = tonumber(redis.call('GET', KEYS[1]) or '')
if cur == nil then return -4 end
if cur ~= version then return -1 end
local top = redis.call('ZREVRANGE', KEYS[4], 0, 0)
if top[1] ~= nil then
  local high = tonumber(top[1])
  if high == nil or high > cur then return -5 end
end
local member = tostring(version)
if redis.call('EXISTS', KEYS[5]) ~= 1 or
   redis.call('HGET', KEYS[3], member) ~= ARGV[2] or
   not redis.call('ZSCORE', KEYS[4], member) or
   redis.call('HLEN', KEYS[5]) ~= file_count then return -9 end
local total_bytes = 0
for i = 7, #ARGV, 2 do
  local file_bytes = string.len(ARGV[i + 1])
  if file_bytes > max_file_bytes then return -8 end
  total_bytes = total_bytes + file_bytes
  if total_bytes > max_total_bytes then return -8 end
  if redis.call('HGET', KEYS[5], ARGV[i]) ~= ARGV[i + 1] then return -9 end
end
redis.call('DEL', KEYS[6])
redis.call('HSET', KEYS[6], unpack(ARGV, 7, #ARGV))
redis.call('RENAME', KEYS[6], KEYS[2])
pcall(function()
  redis.call('LPUSH', KEYS[7], ARGV[3])
  redis.call('LTRIM', KEYS[7], 0, 199)
end)
return version
)";
}

std::string migration_script() {
    return R"(
if (#ARGV - 6) % 2 ~= 0 then return -2 end
if #ARGV < 8 then return -2 end
local version = tonumber(ARGV[1])
local max_files = tonumber(ARGV[4])
local max_file_bytes = tonumber(ARGV[5])
local max_total_bytes = tonumber(ARGV[6])
if version == nil or max_files == nil or max_file_bytes == nil or max_total_bytes == nil then return -2 end
local file_count = (#ARGV - 6) / 2
if file_count < 1 or file_count > max_files then return -8 end
local expected = {'string', 'hash', 'hash', 'zset', 'hash', 'hash', 'list'}
for i = 1, #KEYS do
  local t = redis.call('TYPE', KEYS[i]).ok
  if t ~= expected[i] and t ~= 'none' then return -3 end
end
local cur = tonumber(redis.call('GET', KEYS[1]) or '')
if cur == nil then return -4 end
if cur ~= version then return -1 end
local top = redis.call('ZREVRANGE', KEYS[4], 0, 0)
if top[1] ~= nil then
  local high = tonumber(top[1])
  if high == nil or high > cur then return -5 end
end
local member = tostring(version)
if redis.call('ZCARD', KEYS[4]) ~= 0 or redis.call('HLEN', KEYS[3]) ~= 0 or
   redis.call('EXISTS', KEYS[5]) ~= 0 or
   redis.call('HEXISTS', KEYS[3], member) ~= 0 or
   redis.call('ZSCORE', KEYS[4], member) ~= false then return -6 end
if redis.call('HLEN', KEYS[2]) ~= file_count then return -9 end
local total_bytes = 0
for i = 7, #ARGV, 2 do
  local file_bytes = string.len(ARGV[i + 1])
  if file_bytes > max_file_bytes then return -8 end
  total_bytes = total_bytes + file_bytes
  if total_bytes > max_total_bytes then return -8 end
  if redis.call('HGET', KEYS[2], ARGV[i]) ~= ARGV[i + 1] then return -9 end
end
redis.call('DEL', KEYS[6])
redis.call('HSET', KEYS[6], unpack(ARGV, 7, #ARGV))
redis.call('RENAME', KEYS[6], KEYS[5])
redis.call('HSET', KEYS[3], member, ARGV[2])
redis.call('ZADD', KEYS[4], version, member)
pcall(function()
  redis.call('LPUSH', KEYS[7], ARGV[3])
  redis.call('LTRIM', KEYS[7], 0, 199)
end)
return version
)";
}

std::string orphan_inspect_script() {
    return R"(
local expected = {'string', 'zset', 'hash', 'hash', 'hash'}
for i = 1, #KEYS do
  local t = redis.call('TYPE', KEYS[i]).ok
  if t ~= expected[i] and t ~= 'none' then return {'__error__'} end
end
local current = tonumber(redis.call('GET', KEYS[1]) or '0')
local target = tonumber(ARGV[1])
if current == nil or target == nil or target <= current then return {'__error__'} end
local top = redis.call('ZREVRANGE', KEYS[2], 0, 0)
local index_high = tonumber(top[1] or '0')
if index_high == nil then return {'__error__'} end
local machine_high = 0
for _, value in ipairs(redis.call('HVALS', KEYS[5])) do
  local version = tonumber(string.match(value, '^([^|]+)'))
  if version ~= nil and version > machine_high then machine_high = version end
end
local member = tostring(target)
local indexed = redis.call('ZSCORE', KEYS[2], member) and '1' or '0'
local meta = redis.call('HGET', KEYS[3], member) or '__missing__'
local snapshot_exists = redis.call('EXISTS', KEYS[4]) == 1 and '1' or '0'
local out = {tostring(current), tostring(index_high), tostring(machine_high),
  indexed, meta, snapshot_exists}
if snapshot_exists == '1' then
  for _, value in ipairs(redis.call('HGETALL', KEYS[4])) do table.insert(out, value) end
end
return out
)";
}

std::string restore_version_script() {
    return R"(
if (#ARGV - 7) % 2 ~= 0 or #ARGV < 9 then return -2 end
local expected_current = tonumber(ARGV[1])
local target = tonumber(ARGV[2])
local max_files = tonumber(ARGV[5])
local max_file_bytes = tonumber(ARGV[6])
local max_total_bytes = tonumber(ARGV[7])
if expected_current == nil or target == nil or max_files == nil or
   max_file_bytes == nil or max_total_bytes == nil then return -2 end
local expected = {'string', 'hash', 'zset', 'hash', 'hash', 'hash', 'list', 'hash'}
for i = 1, #KEYS do
  local t = redis.call('TYPE', KEYS[i]).ok
  if t ~= expected[i] and t ~= 'none' then return -3 end
end
local current = tonumber(redis.call('GET', KEYS[1]) or '0')
if current == nil then return -4 end
if current ~= expected_current then return -1 end
if target <= current then return -5 end
local top = redis.call('ZREVRANGE', KEYS[3], 0, 0)
if tonumber(top[1] or '') ~= target then return -5 end
local member = tostring(target)
local file_count = (#ARGV - 7) / 2
if file_count < 1 or file_count > max_files or
   redis.call('EXISTS', KEYS[5]) ~= 1 or
   redis.call('HGET', KEYS[4], member) ~= ARGV[3] or
   not redis.call('ZSCORE', KEYS[3], member) or
   redis.call('HLEN', KEYS[5]) ~= file_count then return -9 end
local total_bytes = 0
for i = 8, #ARGV, 2 do
  local file_bytes = string.len(ARGV[i + 1])
  total_bytes = total_bytes + file_bytes
  if file_bytes > max_file_bytes or total_bytes > max_total_bytes or
     redis.call('HGET', KEYS[5], ARGV[i]) ~= ARGV[i + 1] then return -9 end
end
for _, value in ipairs(redis.call('HVALS', KEYS[8])) do
  local version = tonumber(string.match(value, '^([^|]+)'))
  if version ~= nil and version > target then return -5 end
end
redis.call('DEL', KEYS[6])
redis.call('HSET', KEYS[6], unpack(ARGV, 8, #ARGV))
redis.call('RENAME', KEYS[6], KEYS[2])
redis.call('SET', KEYS[1], target)
pcall(function()
  redis.call('LPUSH', KEYS[7], ARGV[4])
  redis.call('LTRIM', KEYS[7], 0, 199)
end)
return target
)";
}

std::string delete_orphan_script() {
    return R"(
if (#ARGV - 6) % 2 ~= 0 then return -2 end
local expected_current = tonumber(ARGV[1])
local target = tonumber(ARGV[2])
local expected_indexed = ARGV[4]
local expected_snapshot = ARGV[5]
if expected_current == nil or target == nil or
   (expected_indexed ~= '0' and expected_indexed ~= '1') or
   (expected_snapshot ~= '0' and expected_snapshot ~= '1') then return -2 end
local expected = {'string', 'zset', 'hash', 'hash', 'list', 'hash'}
for i = 1, #KEYS do
  local t = redis.call('TYPE', KEYS[i]).ok
  if t ~= expected[i] and t ~= 'none' then return -3 end
end
local current = tonumber(redis.call('GET', KEYS[1]) or '0')
if current == nil then return -4 end
if current ~= expected_current then return -1 end
if target ~= current + 1 then return -5 end
for _, value in ipairs(redis.call('HVALS', KEYS[6])) do
  local version = tonumber(string.match(value, '^([^|]+)'))
  if version ~= nil and version > current then return -5 end
end
local member = tostring(target)
local indexed = redis.call('ZSCORE', KEYS[2], member) and '1' or '0'
local meta = redis.call('HGET', KEYS[3], member) or '__missing__'
local snapshot = redis.call('EXISTS', KEYS[4]) == 1 and '1' or '0'
if indexed ~= expected_indexed or meta ~= ARGV[3] or snapshot ~= expected_snapshot then
  return -9
end
if indexed == '0' and meta == '__missing__' and snapshot == '0' then return -9 end
local file_count = (#ARGV - 6) / 2
if snapshot == '1' then
  if file_count < 1 or redis.call('HLEN', KEYS[4]) ~= file_count then return -9 end
  for i = 7, #ARGV, 2 do
    if redis.call('HGET', KEYS[4], ARGV[i]) ~= ARGV[i + 1] then return -9 end
  end
elseif file_count ~= 0 then return -9 end
redis.call('UNLINK', KEYS[4])
redis.call('HDEL', KEYS[3], member)
redis.call('ZREM', KEYS[2], member)
pcall(function()
  redis.call('LPUSH', KEYS[5], ARGV[6])
  redis.call('LTRIM', KEYS[5], 0, 199)
end)
return 1
)";
}

std::string seed_script() {
    return R"(
if (#ARGV - 4) % 2 ~= 0 then return -2 end
local max_files = tonumber(ARGV[2])
local max_file_bytes = tonumber(ARGV[3])
local max_total_bytes = tonumber(ARGV[4])
if max_files == nil or max_file_bytes == nil or max_total_bytes == nil then return -2 end
if redis.call('TYPE', KEYS[1]).ok ~= 'none' then return 0 end
local expected = {'none', 'hash', 'hash', 'hash', 'zset', 'hash', 'hash'}
for i = 2, #KEYS do
  local t = redis.call('TYPE', KEYS[i]).ok
  if t ~= expected[i] and t ~= 'none' then return -3 end
end
local file_count = (#ARGV - 4) / 2
if file_count > max_files then return -8 end
local total_bytes = 0
for i = 5, #ARGV, 2 do
  local file_bytes = string.len(ARGV[i + 1])
  if file_bytes > max_file_bytes then return -8 end
  total_bytes = total_bytes + file_bytes
  if total_bytes > max_total_bytes then return -8 end
end
if file_count == 0 then
  if redis.call('ZCARD', KEYS[5]) ~= 0 or
     redis.call('HLEN', KEYS[4]) ~= 0 or
     redis.call('EXISTS', KEYS[6]) ~= 0 then return -6 end
  redis.call('DEL', KEYS[2])
  redis.call('SET', KEYS[1], 1)
  return 1
end
if redis.call('ZCARD', KEYS[5]) ~= 0 or
   redis.call('HLEN', KEYS[4]) ~= 0 or
   redis.call('EXISTS', KEYS[6]) ~= 0 then return -6 end
redis.call('DEL', KEYS[7])
redis.call('HSET', KEYS[7], unpack(ARGV, 5, #ARGV))
redis.call('RENAME', KEYS[7], KEYS[6])
redis.call('HSET', KEYS[4], '1', ARGV[1])
redis.call('ZADD', KEYS[5], 1, '1')
redis.call('DEL', KEYS[3])
redis.call('HSET', KEYS[3], unpack(ARGV, 5, #ARGV))
redis.call('RENAME', KEYS[3], KEYS[2])
redis.call('SET', KEYS[1], 1)
return 1
)";
}

}  // namespace config_history
