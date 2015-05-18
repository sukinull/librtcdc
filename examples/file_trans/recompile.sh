#!/bin/bash

cd /home/uniray/Project/librtcdc/src/
make clean
make
sudo cp librtcdc.so /usr/local/lib/
make clean
cd /home/uniray/Project/librtcdc/examples/file_trans/
rm send
rm recv

gcc -luv -lrtcdc signal.c signal.h send_file.c -o send

gcc -luv -lrtcdc signal.h signal.c recv_file.c -o recv