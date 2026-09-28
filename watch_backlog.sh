while true; do
  ts=$(date '+%F %T.%3N')
  tc -s qdisc show dev ens3 | awk -v ts="$ts" '
    /backlog/ {
      pkts=$3
      sub(/p$/, "", pkts)
      if (pkts + 0 > 0) print ts, $0
    }'
  sleep 0.02
done | tee /tmp/qdisc-backlog.log
