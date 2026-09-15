#!/usr/bin/env bash
#
# End-to-end check for the Lender server against the real compose topology:
# MySQL + a NameNode + three DataNodes.
#
# The last phase is the one worth having. It does not simulate a DataNode failure -- it asks
# the running cluster which node holds the 1x-replicated partition, STOPS that container,
# waits for the NameNode to notice, and then re-issues the RPC. That is the only way to show
# that `recreate` is real rather than a branch that merely exists, and it is also the check
# that proves the answer does not change when the cache is lost.
#
# Usage:  bash server/scripts/e2e.sh        (run from lender-service/)
#
# Every phase prints its own exit code. No `&&` chains: a chained probe that returns
# non-zero silently skips everything after it, and a skipped check reports nothing, not pass.

set -u
export MSYS_NO_PATHCONV=1

PROJECT="${PROJECT:-lender}"
COUNTY="${COUNTY:-55025}"          # Dane County, WI -- the largest county in the fixture
BIG="/hdma-wi-2021.parquet"
PART="/partitions/${COUNTY}.parquet"

FAILURES=0
note()  { echo ""; echo "=== $* ==="; }
pass()  { echo "  ok    $*"; }
fail()  { echo "  FAIL  $*"; FAILURES=$((FAILURES + 1)); }

# Run the in-image client against the server, from inside the server container.
cli() { docker compose exec -T server /usr/local/bin/lender_client localhost:5000 "$@"; }

note "bringing the topology up"
docker compose up -d
echo "compose up exit=$?"
docker compose ps

note "waiting for MySQL to finish importing the fixture"
# The import is ~450k rows; it is NOT instant, and querying too early returns a a partial
# count that would silently make every later number wrong.
MYSQL_READY=0
for i in $(seq 1 120); do
  ROWS=$(docker compose exec -T mysql mysql -uroot -pabc -N -B -e \
         "SELECT COUNT(*) FROM CS544.loans" 2>/dev/null | tr -d '[:space:]')
  if [ -n "${ROWS:-}" ] && [ "${ROWS}" -gt 0 ] 2>/dev/null; then
    sleep 3
    ROWS2=$(docker compose exec -T mysql mysql -uroot -pabc -N -B -e \
            "SELECT COUNT(*) FROM CS544.loans" 2>/dev/null | tr -d '[:space:]')
    # Require two equal readings: a growing count means the import is still running.
    if [ "${ROWS}" = "${ROWS2}" ]; then
      MYSQL_READY=1
      echo "  loans row count settled at ${ROWS} (after ~$((i * 3))s)"
      break
    fi
  fi
  sleep 3
done
if [ "$MYSQL_READY" -eq 1 ]; then pass "MySQL up with ${ROWS} loan rows"; else fail "MySQL never became ready"; fi

note "waiting for HDFS to report 3 live DataNodes"
HDFS_READY=0
for i in $(seq 1 60); do
  LIVE=$(docker compose exec -T nn bash -c \
    'export CLASSPATH=$($HADOOP_HOME/bin/hdfs classpath --glob); hdfs dfsadmin -fs hdfs://boss:9000 -report 2>/dev/null | grep -c "^Name:"' \
    2>/dev/null | tr -d '[:space:]')
  if [ "${LIVE:-0}" = "3" ]; then
    HDFS_READY=1
    echo "  3 live DataNodes (after ~$((i * 3))s)"
    break
  fi
  sleep 3
done
if [ "$HDFS_READY" -eq 1 ]; then pass "HDFS up with 3 DataNodes"; else fail "HDFS never reported 3 DataNodes (saw '${LIVE:-none}')"; fi

note "PHASE 1 -- DbToHdfs"
OUT=$(cli DbToHdfs); RC=$?
echo "$OUT"
echo "exit=$RC"
if [ $RC -eq 0 ]; then pass "DbToHdfs succeeded"; else fail "DbToHdfs exit $RC"; fi

note "PHASE 2 -- BlockLocations on the 2x-replicated big file"
OUT=$(cli BlockLocations "$BIG"); RC=$?
echo "$OUT"
echo "exit=$RC"
NODES=$(echo "$OUT" | grep -c '^[^#]')
REPLICAS=$(echo "$OUT" | awk '/^#/ {print $4}')
if [ $RC -eq 0 ]; then pass "BlockLocations returned a map"; else fail "BlockLocations exit $RC"; fi
if [ "${NODES:-0}" -gt 1 ]; then
  pass "blocks are spread over ${NODES} DataNodes (1 MB block size is doing its job)"
else
  fail "expected more than one DataNode, saw ${NODES:-0}"
fi

note "PHASE 3 -- CalcAvgLoan, cold cache (expect: create)"
OUT=$(cli CalcAvgLoan "$COUNTY"); RC=$?
echo "$OUT"
AVG_CREATE=$(echo "$OUT" | sed -n '1p')
SRC_CREATE=$(echo "$OUT" | sed -n '2p')
if [ "$SRC_CREATE" = "create" ]; then pass "source=create on a cold cache"; else fail "expected create, got '${SRC_CREATE}'"; fi

note "PHASE 4 -- CalcAvgLoan again, warm cache (expect: reuse)"
OUT=$(cli CalcAvgLoan "$COUNTY"); RC=$?
echo "$OUT"
AVG_REUSE=$(echo "$OUT" | sed -n '1p')
SRC_REUSE=$(echo "$OUT" | sed -n '2p')
if [ "$SRC_REUSE" = "reuse" ]; then pass "source=reuse on a warm cache"; else fail "expected reuse, got '${SRC_REUSE}'"; fi
if [ "$AVG_CREATE" = "$AVG_REUSE" ]; then pass "the cached answer equals the computed one (${AVG_REUSE})"; else fail "cache disagrees: ${AVG_CREATE} vs ${AVG_REUSE}"; fi

note "PHASE 5 -- fault injection: stop the DataNode holding the 1x partition"
echo "partition block locations:"
PARTOUT=$(cli BlockLocations "$PART"); echo "$PARTOUT"
HOLDER=$(echo "$PARTOUT" | grep -v '^#' | head -1 | awk '{print $1}')
echo "holder hostname: '${HOLDER}'"

VICTIM=""
if [ -n "${HOLDER}" ]; then
  # HDFS reports the DataNode's container hostname, which for a compose replica is the
  # short container id. Map it back to a container name so it can be stopped.
  VICTIM=$(docker ps --format '{{.ID}} {{.Names}}' | awk -v h="${HOLDER}" 'index(h, $1) == 1 {print $2}' | head -1)
  if [ -z "${VICTIM}" ]; then
    VICTIM=$(docker ps --format '{{.Names}}' | grep -- "-dn-" | head -1)
    echo "  (could not map '${HOLDER}' to a container; falling back to ${VICTIM})"
  fi
fi

if [ -n "${VICTIM}" ]; then
  echo "stopping ${VICTIM}"
  docker stop "${VICTIM}" > /dev/null; echo "stop exit=$?"
  # The NameNode is started with stale.datanode.interval=10000 and
  # heartbeat.recheck-interval=30000, so it takes roughly 30-45s to mark the node dead.
  echo "waiting for the NameNode to mark it dead..."
  for i in $(seq 1 30); do
    LIVE=$(docker compose exec -T nn bash -c \
      'export CLASSPATH=$($HADOOP_HOME/bin/hdfs classpath --glob); hdfs dfsadmin -fs hdfs://boss:9000 -report 2>/dev/null | grep -c "^Name:"' \
      2>/dev/null | tr -d '[:space:]')
    if [ "${LIVE:-3}" = "2" ]; then echo "  down to 2 live DataNodes (after ~$((i * 5))s)"; break; fi
    sleep 5
  done

  note "PHASE 6 -- CalcAvgLoan with the partition's node down (expect: recreate)"
  OUT=$(cli CalcAvgLoan "$COUNTY"); RC=$?
  echo "$OUT"
  AVG_RECREATE=$(echo "$OUT" | grep -E '^[0-9]+$' | head -1)
  SRC_RECREATE=$(echo "$OUT" | grep -E '^(create|reuse|recreate)$' | head -1)
  if [ "$SRC_RECREATE" = "recreate" ]; then
    pass "source=recreate after losing the partition's only replica"
  else
    fail "expected recreate, got '${SRC_RECREATE}'"
  fi
  # THE POINT OF THE WHOLE DESIGN: the answer must be unchanged. The big file survived on
  # its second replica, so the rebuilt partition must agree with the original.
  if [ -n "${AVG_RECREATE}" ] && [ "$AVG_RECREATE" = "$AVG_CREATE" ]; then
    pass "the answer survived the failure unchanged (${AVG_RECREATE})"
  else
    fail "answer changed across the failure: ${AVG_CREATE} -> ${AVG_RECREATE:-none}"
  fi

  echo "restarting ${VICTIM}"
  docker start "${VICTIM}" > /dev/null; echo "start exit=$?"
else
  fail "could not identify a DataNode to stop -- fault injection SKIPPED (not passed)"
fi

note "SUMMARY"
echo "failures: ${FAILURES}"
if [ "${FAILURES}" -eq 0 ]; then echo "E2E_RESULT=PASS"; else echo "E2E_RESULT=FAIL"; fi
exit "${FAILURES}"
