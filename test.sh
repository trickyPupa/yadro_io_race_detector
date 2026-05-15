MODULE="dm-race-detector"
IMG_FILE="./test_backend.img"
DEV_NAME="test0"

# файл диска
dd if=/dev/zero of=$IMG_FILE bs=1M count=128

LOOP=$(sudo losetup --show -f $IMG_FILE)

# загрзука модуля
sudo insmod $MODULE.ko

# создание dm устройства
echo "0 $(sudo blockdev --getsz $LOOP) race-detector $LOOP" | sudo dmsetup create $DEV_NAME

# запись 1 [128, 191]
sudo dd oflag=direct if=/dev/urandom of=/dev/mapper/$DEV_NAME bs=512 count=64 seek=128 &

# запись 2 [136, 151]
sudo dd oflag=direct if=/dev/urandom of=/dev/mapper/$DEV_NAME bs=512 count=16 seek=136

# проверка
sleep 0.5
sudo dmesg | tail | grep "race detected"

# очистка 
sudo dmsetup remove $DEV_NAME 
sudo losetup -d $LOOP
rm -f $IMG_FILE
sudo rmmod $MODULE
