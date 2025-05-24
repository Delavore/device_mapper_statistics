obj-m += dm_proxy.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

insert: 
	insmod dm_proxy.ko
	dmsetup create dmp1 --table "0 16 dmp /dev/mapper/zero1 0"

delete:
	dmsetup remove dmp1 
	rmmod dm_proxy
