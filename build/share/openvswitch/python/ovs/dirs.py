import os
PKGDATADIR = os.environ.get("OVS_PKGDATADIR", """/home/xshl5/workspace/openvswitch-2.5.12/build/share/openvswitch""")
RUNDIR = os.environ.get("OVS_RUNDIR", """/home/xshl5/workspace/openvswitch-2.5.12/build/var/run/openvswitch""")
LOGDIR = os.environ.get("OVS_LOGDIR", """/home/xshl5/workspace/openvswitch-2.5.12/build/var/log/openvswitch""")
BINDIR = os.environ.get("OVS_BINDIR", """/home/xshl5/workspace/openvswitch-2.5.12/build/bin""")

DBDIR = os.environ.get("OVS_DBDIR")
if not DBDIR:
    sysconfdir = os.environ.get("OVS_SYSCONFDIR")
    if sysconfdir:
        DBDIR = "%s/openvswitch" % sysconfdir
    else:
        DBDIR = """/home/xshl5/workspace/openvswitch-2.5.12/build/etc/openvswitch"""
