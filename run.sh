make clean
make
./rufs -s /tmp/yw1017/mountdir
cd benchmark
make clean
make
./test_case
cd ..
fusermount -u /tmp/yw1017/mountdir