
# DPDK testpmd with Shared MAC and Virtual Storage profile

The purpose of this write-up is to show the steps for running the testpmd app with shared mac
on LS1046 and to capture traffic using the pdump application.

## Bill of materials

  - one LS1046RDB board connected to test center (through fm1-mac5 interface)



## Steps for building the application
- Download the toolchain from Linaro
```sh
wget https://releases.linaro.org/components/toolchain/binaries/7.2-2017.11/aarch64-linux-gnu/gcc-linaro-7.2.1-2017.11-x86_64_aarch64-linux-gnu.tar.xz
tar -xvf gcc-linaro-7.2.1-2017.11-i686_aarch64-linux-gnu.tar.xz
```
- Set the environment
```sh
export PATH=$PATH:/<path_to_toolchain>/gcc-linaro-7.2.1-2017.11-x86_64_aarch64-linux-gnu/bin/
export RTE_SDK=/<path_to_dpdk>/
export RTE_TARGET=arm64-dpaa-linuxapp-gcc
export CROSS=aarch64-linux-gnu-
```

- Generate the config
```sh
make config T=arm64-dpaa-linuxapp-gcc
```

**Note:** libpcap must be compiled in order to have support for pdump
It can be cloned from: https://github.com/the-tcpdump-group/libpcap.git

-Build testpmd & pdump applications
```sh
make -j 12 CONFIG_RTE_KNI_KMOD=n CONFIG_RTE_EAL_IGB_UIO=n \
  CONFIG_RTE_LIBRTE_PMD_PCAP=y CONFIG_RTE_LIBRTE_PDUMP=y \
  C_INCLUDE_PATH=/work/libpcap/install/usr/local/include/ \
  LDFLAGS=-L/work/libpcap/install/usr/local/lib/
```

**The output binaries can be found in:**
< path_to_dpdk >/build/app/testpmd
< path_to_dpdk >/build/app/dpdk-pdump

- Cleaning the build
```sh
make clean -j 12
```
- Building for debug purposes
```sh
export EXTRA_CFLAGS=”-O0 -g”
make clean -j 12
make -j 12 CONFIG_RTE_KNI_KMOD=n CONFIG_RTE_EAL_IGB_UIO=n \
  CONFIG_RTE_LIBRTE_PMD_PCAP=y CONFIG_RTE_LIBRTE_PDUMP=y \
  C_INCLUDE_PATH=/work/libpcap/install/usr/local/include/ \
  LDFLAGS=-L/work/libpcap/install/usr/local/lib/
```

## Changes in the dts
For this use case SDK 1703 was tested.
The changes will consist in adding shared mac port in the main dts, and vsp configuration
in the dtsi file as follows:

```sh

diff --git a/arch/arm64/boot/dts/freescale/fsl-ls1046a.dtsi b/arch/arm64/boot/dts/freescale/fsl-ls1046a.dtsi
index 7b3019a..a69dd0e 100644
--- a/arch/arm64/boot/dts/freescale/fsl-ls1046a.dtsi
+++ b/arch/arm64/boot/dts/freescale/fsl-ls1046a.dtsi
@@ -57,6 +57,27 @@
                crypto = &crypto;
        };

+       chosen {
+                name = "chosen";
+
+                dpaa-extended-args {
+                        fman0-extd-args {
+                                cell-index = <0>;
+                                compatible = "fsl,fman-extended-args";
+                                dma-aid-mode = "port";
+
+
+                                fman0_rx4-extd-args {
+                                        cell-index = <4>;
+                                        compatible = "fsl,fman-port-1g-rx-extended-args";
+                                        /* Define Virtual storage profile */
+                                        /* <number of profiles, default profile id> */
+                                        vsp-window = <2 0>;
+                                };
+                        };
+                };
+        };
+
        cpus {
                #address-cells = <1>;
                #size-cells = <0>;
```
**Note:** If the user wants more storage profiles (e.g: there are 4 flows and each flow has its own pool, then vsp-window will be changed with <4 0>)

Below, a user will notice that there is only one port available as private netdev in dts (all the others will be configured by proxy driver. For all the other ports no qbman portal will be used which is as expected. qbman portals will be used by the DPDK receive/ transmit queues in testpmd and by the secondary process - pdump, used for capturing DPDK classified packets)

```sh
diff --git a/arch/arm64/boot/dts/freescale/fsl-ls1046a-rdb-usdpaa.dts b/arch/arm64/boot/dts/freescale/fsl-ls1046a-rdb-usdpaa.dts
index 6f381fe..4ed29d4 100644
--- a/arch/arm64/boot/dts/freescale/fsl-ls1046a-rdb-usdpaa.dts
+++ b/arch/arm64/boot/dts/freescale/fsl-ls1046a-rdb-usdpaa.dts
@@ -32,6 +32,15 @@
                fsl,bpool-thresholds = <0x100 0x300 0x0 0x0>;
        };

+
+bp16: buffer-pool@16 {
+                        compatible = "fsl, ls1046-bpool", "fsl,bpool";
+                        fsl,bpid = <16>;
+                        fsl,bpool-ethernet-cfg = <0 2048 0 1728 0 0>;
+                        fsl,bpool-thresholds = <0x100 0x300 0x0 0x0>;
+       };
+
+
        fsl,dpaa {
                compatible = "fsl,ls1046a", "fsl,dpaa", "simple-bus";

@@ -50,10 +59,11 @@
                };

                ethernet@4 {
-                       compatible = "fsl,dpa-ethernet-init";
-                       fsl,bman-buffer-pools = <&bp7 &bp8 &bp9>;
-                       fsl,qman-frame-queues-rx = <0x58 1 0x59 1>;
-                       fsl,qman-frame-queues-tx = <0x78 1 0x79 1>;
+
+                       compatible = "fsl,ls1046-dpa-ethernet-shared", "fsl,dpa-ethernet-shared";
+                       fsl,bman-buffer-pools = <&bp16>;
+                       fsl,qman-frame-queues-rx = <0x6e 1 0x6f 1 0x2000 3>;
+                       fsl,qman-frame-queues-tx = <0 1 0 1 0x3000 8>;
                };
```

- Build the device tree
## Running the use case
- 	Boot the target (using a SDK 1703 image) and open two consoles:
In one console execute:
```sh
#this command is run only once. It’s mandatory in this context(testpmd with pdump)
echo 0 | tee /proc/sys/kernel/randomize_va_space
#this command is executed only once (folder creation)
mkdir /mnt/hugepages

mount -t hugetlbfs none /mnt/hugepages
export DPAA_NUM_RX_QUEUES=4
export DPAA_FMC_MODE=1

fmc -c usdpaa_config_ls1046_vsp.xml -p usdpaa_policy_custom_vsp.xml -a

#run the application

 ./testpmd-new  --lcores='0,1,2,3,4@0'  \
--master-lcore 0 -n 6 --log-level 100 -- \
-i  --nb-cores=4  --portmask=0x4 --nb-ports=1  \
--total-num-mbufs=1025 \
--forward-mode=io --rxq=4 --txq=4 \
```

**Note: Port mask 4 means that port port id 2 is used – this is the shared mac.**

In the second console run the dpdk-pdump:
```sh
#rx-dev path can be changed; it’s not mandatory to be in tmp

ifconfig fm1-mac5 <ip addr> up

./dpdk-pdump -- --pdump 'port=2,queue=*,rx-dev=/tmp/capture.pcap'

#Output
EAL: Detected 4 lcore(s)
EAL: DPAA Bus Detected
EAL: Probing VFIO support...
EAL: VFIO support initialized
EAL: PCI device 0002:01:00.0 on NUMA socket 0
EAL:   probe driver: 8086:10d3 net_e1000_em
dpaa_sec: Device already init by primary process
dpaa_sec: Device already init by primary process
dpaa_sec: Device already init by primary process
dpaa_sec: Device already init by primary process
PMD: Initializing pmd_pcap for net_pcap_rx_0
PMD: Creating pcap-backed ethdev on numa socket -1
Port 1 MAC: 00 00 00 01 02 03
```
**NOTE:**
**First run the testpmd application then start the pdump. Thus the order is like this:**
**1.	testpmd**
**2.	pdump**

**Also after starting the testpmd do not forget to bring up the fm1-mac5 port.**

Start injecting traffic in the testpmd port. Traffic that is classified will reach the cores and will be captured by pdump.
If you want to see the captured packets, in pdump console hit CTRL+C and you will notice an output like:

```
Signal 2 received, preparing to exit...
##### PDUMP DEBUG STATS #####
-packets dequeued:                     1964
-packets transmitted to vdev:          1964
-packets freed:                        0
```
If you want to restart pdump, you will first need to stop testpmd. The steps are as follows:

1.	quit testpmd
2.	start testpmd
3.	start pdump


**For each capture, restart the testpmd then start pdump.**


**Additional info:**

When you start pdump you can do the following change(with bold)
./dpdk-pdump  --  --pdump 'port=2,**queue=0**,rx-dev=/tmp/capture.pcap'

It means you can dump frames from a specific queue. In this case you can make sure that classification works on that specific queue.
If you send traffic that classifies on queue 1 and queue 0 was set for dump, you will not see any capture.
That means that the debug prints can be removed and use only pdump for checking.

Remember that(check also the PCD file) :
Queue 0 = 0x900
[…]
Queue 3 = 0x903

**Use case model:**
```
     +-----------------+
     |      DPDK       |
     |                 |
     |C1) (C2) (C3) (C4|  (Ci) = logical DPDK core each mapped to a physical core
     |Q0  FQ1  FQ2  FQ3|
     +-----------------+
     |    PCD MODEL    |
     +-----------------+
     |  FMAN@Memac5    |
     +---^--^-^-^------+
         |  | | |
         +  + + +
traffic flows (traffic that match the classification model go to DPDK, the rest of traffic goes in the stack)


```

## How to validate the shared mac:

User will send different patterns of traffic and will notice the following behavior:

**Scenario1 :**
sending 7 flows with the following specs(note: if MSG_ID is replaced with 0x00 the same result should be observed):
```
key: UDP       | MSG_ID |      DSP_ID
     0x1092      7e              00
     0x1092      7e              01
     0x1092      7e              02
     0x1092      7e              03
     0x1092      7e              04
     0x1092      7e              05
     0x1092      7e              06
```
**Result**(you can validate with pdump as mentioned earlier if there are no console prints):
```
PMD:  FQID = 0x901 on cpu (2)
PMD:  FQID = 0x901 on cpu (2)
PMD:  FQID = 0x902 on cpu (3)
PMD:  FQID = 0x902 on cpu (3)
PMD:  FQID = 0x900 on cpu (1)
PMD:  FQID = 0x900 on cpu (1)
PMD:  FQID = 0x903 on cpu (0)
```

**Interpretation:**
```
0x1092      7e              00  goes to 0x900 on cpu 1
0x1092      7e              01  goes to 0x901 on cpu 2
0x1092      7e              02  goes to 0x902 on cpu 3
0x1092      7e              03  goes to 0x903 on cpu 0
0x1092      7e              04  goes to 0x900 on cpu 1
0x1092      7e              05  goes to 0x901 on cpu 2
0x1092      7e              06 g oes to 0x902 on cpu 3
```
**Note:**
Core 0 will be set as master core, though we can use a logical core number equivalent to Core 0 to have a consumer thread on Core 0 too,
thus to use all the cores from the system, each core with a lcore thread that consumes.

In case pdump is used, the user must be careful when defining consumer threads. This is because
each consumer thread will initialize a tx / rx queue pair which can consume bman portal.
On LS1046 only 8 portals are available. If all of them are consumed, pdump application will not
be able to capture frames because will not be able to initialize bman portals for buffer allocation/ deallocation.

**Scenario 2 :**
Sending traffic that does not match the udp dport or any of the msg_id dsp_id –(user can choose different configurations)

**Result:**
tcpdump will capture the traffic that does not reach DPDK.

Interpretation:
As it can be observed from the xml file, there are some “miss queues”. (shared MAC Rx queues). Traffic that does not match the classification, will go to the stack.

**Scenario 3 :**
-from Linux execute the following commands:
```
arp -s 10.0.0.2 0:0:0:0:0:2 dev fm1-mac5

ping 10.0.0.2
```
**Result:**
  In traffic generator you should notice echo request packets that arrive on RX
  
## Final notes
- the current repo contains also the Spirent TCC traffic file and the PCD files
- 

