#!/system/bin/sh
HERE="$(cd "$(dirname "$0")" && pwd)"
export ASAN_OPTIONS=log_to_syslog=false,allow_user_segv_handler=1,fast_unwind_on_malloc=1,detect_leaks=0
ASAN_LIB=$(ls "$HERE"/libclang_rt.asan-*.so 2>/dev/null || echo "")
if [ -n "$ASAN_LIB" ]; then
  export LD_PRELOAD="$ASAN_LIB"
fi
exec "$@"
