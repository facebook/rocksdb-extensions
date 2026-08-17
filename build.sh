CLEAN_BUILD_DIRECTORY="${CLEAN_BUILD_DIRECTORY:-OFF}"
export CLEAN_BUILD_DIRECTORY
./scripts/build-latest-releases.sh 2>&1 | tee /tmp/b.log
sleep 10000
