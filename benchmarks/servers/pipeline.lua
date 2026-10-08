-- wrk script for the pipelined plaintext case (F2): every write sends `depth`
-- requests back to back (default 16; `-- 16` after the URL sets it), and
-- done() prints one JSON line the runner reads. The runner uses the throughput
-- only: wrk 4.2's latency percentiles are wrong under pipelining.
local request_batch

init = function(args)
  local depth = tonumber(args[1]) or 16
  local requests = {}
  for i = 1, depth do
    requests[i] = wrk.format()
  end
  request_batch = table.concat(requests)
end

request = function()
  return request_batch
end

done = function(summary, latency, requests)
  local errors = summary.errors
  io.write(string.format(
    '{"responses":%d,"durationUs":%d,"errors":%d,"p50Us":%d,"p99Us":%d,"p999Us":%d}\n',
    summary.requests, summary.duration,
    errors.connect + errors.read + errors.write + errors.status + errors.timeout,
    latency:percentile(50), latency:percentile(99), latency:percentile(99.9)))
end
