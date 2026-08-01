#!/usr/bin/env bash
set -euo pipefail

client_binary=$1 # 待测试的客户端程序路径。
server_binary=$2 # 待测试的服务端程序路径。
temporary_directory=$(mktemp -d) # 本次测试独占的数据目录。
port=$((20000 + $$ % 20000)) # 根据进程号选择冲突概率较低的测试端口。
client_pid="" # 客户端进程号。
server_pid="" # 服务端进程号。

# 在限定时间内停止指定测试进程。
stop_process() {
    local pid=$1
    if [[ -z "$pid" ]] || ! kill -0 "$pid" 2>/dev/null; then
        return
    fi
    kill -TERM "$pid" 2>/dev/null || true
    for _ in $(seq 1 50); do
        if ! kill -0 "$pid" 2>/dev/null; then
            wait "$pid" 2>/dev/null || true
            return
        fi
        sleep 0.1
    done
    kill -KILL "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}

# 清理测试进程和临时文件。
cleanup() {
    stop_process "$client_pid"
    stop_process "$server_pid"
    rm -rf -- "$temporary_directory"
}
trap cleanup EXIT

# 等待输出文件出现指定文本。
wait_for_text() {
    local file=$1
    local expected=$2
    for _ in $(seq 1 100); do
        if grep -Fq "$expected" "$file" 2>/dev/null; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

touch "$temporary_directory/app.log"
"$server_binary" \
    "$port" "$temporary_directory/server/dedup.state" \
    >"$temporary_directory/server.out" 2>&1 &
server_pid=$!

"$client_binary" \
    "$temporary_directory/app.log" 127.0.0.1 "$port" \
    "$temporary_directory/client" \
    >"$temporary_directory/client.out" 2>&1 &
client_pid=$!

printf 'e2e-first\ne2e-second\n' >>"$temporary_directory/app.log"
if ! wait_for_text "$temporary_directory/server.out" "e2e-second"; then
    printf '%s\n' '端到端测试失败：服务端没有收到初始日志'
    cat "$temporary_directory/client.out"
    cat "$temporary_directory/server.out"
    exit 1
fi

mv "$temporary_directory/app.log" "$temporary_directory/app.log.1"
printf 'e2e-old-tail\n' >>"$temporary_directory/app.log.1"
printf 'e2e-after-rotate\n' >"$temporary_directory/app.log"
if ! wait_for_text "$temporary_directory/server.out" "e2e-after-rotate" ||
   ! wait_for_text "$temporary_directory/server.out" "e2e-old-tail"; then
    printf '%s\n' '端到端测试失败：日志轮转后存在消息丢失'
    cat "$temporary_directory/client.out"
    cat "$temporary_directory/server.out"
    exit 1
fi

stop_process "$client_pid"
client_pid=""
if [[ -s "$temporary_directory/client/pending.wal" ]]; then
    printf '%s\n' '端到端测试失败：ACK 后 WAL 仍有待确认消息'
    exit 1
fi

stop_process "$server_pid"
server_pid=""
grep -Fq 'LogBridge 已停止' "$temporary_directory/client.out"
grep -Fq 'LogBridge 服务端已安全停止' "$temporary_directory/server.out"
printf '%s\n' 'End-to-end test passed'
