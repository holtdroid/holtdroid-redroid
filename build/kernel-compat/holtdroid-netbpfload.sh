export LD_PRELOAD=/system/lib64/libholtdroid_uname.so
exec /apex/com.android.tethering/bin/netbpfload "$@"
