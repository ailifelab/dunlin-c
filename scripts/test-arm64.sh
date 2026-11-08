#!/bin/sh
# Dedicated disposable container. Only its ID is removed by the exit trap.
set -eu
case ${1:-bionic} in
 bionic) suite=bionic; image=ubuntu@sha256:152dc042452c496007f07ca9127571cb9c29697f42acbfad72324b2bb2e43c98 ;;
 noble) suite=noble; image=ubuntu@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852 ;;
 *) echo "usage: $0 [bionic|noble]" >&2; exit 64 ;;
esac
root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
out="$root/dist/0.2.12/$suite-arm64"
report="$root/reports/0.2.12/$suite"
mkdir -p "$out" "$report"
set --
for item in Makefile cli include kernel platform plugins runtime tests scripts/qwen-chat.sh; do
    set -- "$@" --mount "type=bind,source=$root/$item,target=/src/$item,readonly"
done
id=$(docker run -d --init --platform linux/arm64 --name "dunlin-test-$suite-$$" \
    --cpus 2 --memory 1536m --pids-limit 160 --security-opt no-new-privileges \
    "$@" -w /src "$image" sleep infinity)
trap 'docker rm -f "$id" >/dev/null' EXIT
trap 'exit 130' INT TERM
echo "[$suite] installing distro packages in dedicated ARM64 container"
docker exec "$id" sh -ec '
    apt-get update -o Acquire::Retries=2
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends build-essential pkg-config libjson-c-dev libcurl4-openssl-dev libedit-dev ncurses-base curl python3 ca-certificates file
' >"$report/packages.log" 2>&1 || { tail -40 "$report/packages.log" >&2; exit 1; }
docker network disconnect bridge "$id"
{
    echo "base=$image"
    docker inspect "$id" --format 'image_id={{.Image}} cpus_nano={{.HostConfig.NanoCpus}} memory_bytes={{.HostConfig.Memory}}'
    docker exec "$id" sh -ec 'cat /etc/os-release; uname -sm; gcc --version; getconf GNU_LIBC_VERSION; dpkg-query -W gcc make libc6 libasan* libubsan* libcurl4* libjson-c*; python3 --version'
} > "$report/environment.txt"
run_test() {
    log=$1
    shift
    echo "[$suite] $log"
    docker exec -u nobody -e HOME=/tmp -e PYTHONDONTWRITEBYTECODE=1 "$id" "$@" >"$report/$log" 2>&1 ||
        { tail -60 "$report/$log" >&2; exit 1; }
}
run_test test.log make -j2 BUILD=/tmp/release test
run_test sanitize.log env ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    make -j2 BUILD=/tmp/instrumented CFLAGS="-std=c11 -Wall -Wextra -Wpedantic -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-pie" LDFLAGS="-no-pie" test
docker exec -e PYTHONDONTWRITEBYTECODE=1 "$id" python3 tests/runtime_closure.py /tmp/release/dunlin > "$report/runtime-closure.txt" 2>&1 ||
    { cat "$report/runtime-closure.txt" >&2; exit 1; }
run_test resource.txt sh -ec 'cc -O2 tests/measure.c -o /tmp/measure && python3 tests/benchmark.py /tmp/measure /tmp/release/dunlin'
run_test binary.txt sh -ec 'file /tmp/release/dunlin; readelf -h /tmp/release/dunlin; readelf -d /tmp/release/dunlin; ldd /tmp/release/dunlin; sha256sum /tmp/release/dunlin'
for file in dunlin libdunlin.a libdunlin-kernel.a; do
    docker cp "$id:/tmp/release/$file" "$out/$file"
done
docker cp "$id:/tmp/release/kernel.trace" "$report/kernel.trace"
echo "[$suite] passed. $out"
