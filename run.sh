cd /tmp/yw1017/mountdir/
rm -rf *
cd ~/project4-release
rm -rf DISKFILE
cd benchmark
make clean
make
./simple_test