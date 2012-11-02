#
# Copyright (C)  Heinz Mauelshagen, 2004-2010 Red Hat GmbH. All rights reserved.
#
# See file LICENSE at the top of this source tree for license information.
#

Summary: dmraid (Device-mapper RAID tool and library)
Name: dmraid
Version: 1.0.0.rc16
Release: 6%{?dist}
License: GPLv2+
Group: System Environment/Base
URL: http://people.redhat.com/heinzm/sw/dmraid
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-root-%(%{__id_u} -n)
BuildRequires: device-mapper-devel >= 1.02.02-2
BuildRequires: device-mapper-event-devel
BuildRequires: libselinux-devel
BuildRequires: libsepol-devel
Requires: device-mapper >= 1.02.02-2
Requires: dmraid-events
Requires: kpartx
Obsoletes: dmraid-libs < %{version}-%{release}
Provides: dmraid-libs = %{version}-%{release}
Source: ftp://people.redhat.com/heinzm/sw/dmraid/src/%{name}-%{version}.tar.bz2

%description
DMRAID supports RAID device discovery, RAID set activation, creation,
removal, rebuild and display of properties for ATARAID/DDF1 metadata on
Linux >= 2.4 using device-mapper.

%package -n dmraid-devel
Summary: Development libraries and headers for dmraid.
Group: Development/Libraries
Requires: dmraid = %{version}-%{release}, sgpio

%description -n dmraid-devel
dmraid-devel provides a library interface for RAID device discovery,
RAID set activation and display of properties for ATARAID volumes.

%package -n dmraid-events
Summary: dmevent_tool (Device-mapper event tool) and DSO
Group: System Environment/Base
Requires: dmraid = %{version}-%{release}, sgpio
Requires: device-mapper-event

%description -n dmraid-events
Provides a dmeventd DSO and the dmevent_tool to register devices with it
for device monitoring.  All active RAID sets should be manually registered
with dmevent_tool.

%package -n dmraid-events-logwatch
Summary: dmraid logwatch-based email reporting
Group: System Environment/Base
Requires: dmraid-events = %{version}-%{release}, logwatch, /etc/cron.d

%description -n dmraid-events-logwatch
Provides device failure reporting via logwatch-based email reporting.
Device failure reporting has to be activated manually by activating the 
/etc/cron.d/dmeventd-logwatch entry and by calling the dmevent_tool
(see manual page for examples) for any active RAID sets.

%prep
%setup -q -n dmraid/%{version}

%build
%define _libdir /%{_lib}

%configure --prefix=/usr --sbindir=/sbin --libdir=%{_libdir} --mandir=%{_mandir} --includedir=%{_includedir} --enable-debug --disable-static_link --enable-led --enable-intel_led
make

%install
rm -rf $RPM_BUILD_ROOT
install -m 755 -d $RPM_BUILD_ROOT{%{_libdir},/sbin,%{_sbindir},%{_bindir},%{_libdir},%{_includedir}/dmraid/,/var/lock/dmraid,/etc/cron.d/,/etc/logwatch/conf/services/,/etc/logwatch/scripts/services/,/var/cache/logwatch/dmeventd/}
make DESTDIR=$RPM_BUILD_ROOT install
ln -s dmraid $RPM_BUILD_ROOT/sbin/dmraid.static

# Provide convenience link from dmevent_tool
(cd $RPM_BUILD_ROOT/sbin ; ln -f dmevent_tool dm_dso_reg_tool)
(cd $RPM_BUILD_ROOT/%{_mandir}/man8 ; ln -f dmevent_tool.8 dm_dso_reg_tool.8 ; ln -f dmraid.8 dmraid.static.8)

install -m 644 include/dmraid/*.h $RPM_BUILD_ROOT/%{_includedir}/dmraid/

# Install logwatch config file and script for dmeventd
install -m 644 logwatch/dmeventd.conf $RPM_BUILD_ROOT/etc/logwatch/conf/services/dmeventd.conf
install -m 755 logwatch/dmeventd $RPM_BUILD_ROOT/etc/logwatch/scripts/services/dmeventd
install -m 644 logwatch/dmeventd_cronjob.txt $RPM_BUILD_ROOT/etc/cron.d/dmeventd-logwatch
install -m 0700 /dev/null $RPM_BUILD_ROOT/var/cache/logwatch/dmeventd/syslogpattern.txt

rm -f $RPM_BUILD_ROOT/%{_libdir}/libdmraid.a

%clean
rm -rf $RPM_BUILD_ROOT

%post -p /sbin/ldconfig

%postun -p /sbin/ldconfig

%files
%defattr(-,root,root)
%doc CHANGELOG CREDITS KNOWN_BUGS LICENSE LICENSE_GPL LICENSE_LGPL README TODO doc/dmraid_design.txt
/%{_mandir}/man8/dmraid*
/sbin/dmraid
/sbin/dmraid.static
%{_libdir}/libdmraid.so*
%{_libdir}/libdmraid-events-isw.so*
/var/lock/dmraid

%files -n dmraid-devel
%defattr(-,root,root)
%dir %{_includedir}/dmraid
%{_includedir}/dmraid/*

%files -n dmraid-events
%defattr(-,root,root)
/%{_mandir}/man8/dmevent_tool*
/%{_mandir}/man8/dm_dso_reg_tool*
/sbin/dmevent_tool
/sbin/dm_dso_reg_tool

%files -n dmraid-events-logwatch
%defattr(-,root,root)
%config(noreplace) /etc/logwatch/*
%config(noreplace) /etc/cron.d/dmeventd-logwatch
%dir /var/cache/logwatch/dmeventd
%ghost /var/cache/logwatch/dmeventd/syslogpattern.txt

%changelog
* Mon May 31 2010  Heinz Mauelshagen <heinzm@redhat.com> - 1.0.0.rc16-6
- remove superfluous libselinux/libsepol configure options

* Tue Jan 12 2010  Heinz Mauelshagen <heinzm@redhat.com> - 1.0.0.rc16-5
- Support DESTDIR in all Makefiles
- Fix handling spares in RAID names in vendor metadata

* Tue Jan 12 2010  Heinz Mauelshagen <heinzm@redhat.com> - 1.0.0.rc16-4
- Change dmraid DSO version to "1" and allow for display of 
  extended internal library version

* Tue Jan 12 2010  Heinz Mauelshagen <heinzm@redhat.com> - 1.0.0.rc16-3
- Add logwatch files and move pattern file to /var/cache
- Fix multiple options (eg. "-ccc") not recognized properly

* Mon Nov 2 2009  Heinz Mauelshagen <heinzm@redhat.com> - 1.0.0.rc16-2
- Fix manual path in specfile
- fix manual pages for dmraid.static and dm_dso_reg_tool
- ddf1 metadata format handler LSI persistent name fix
- fix pdc metadata format handler to report the correct number
  of devices in a RAID10 subset
- move libraries to /lib* in order to avoid catch22
  with unmountable /usr

* Wed Oct 09 2008 Heinz Mauelshagen <heinzm@redhat.com> - 1.0.0.rc16-1
- Updated

* Wed Sep 17 2008 Heinz Mauelshagen <heinzm@redhat.com> - 1.0.0.rc15
- Added support for RAID set create/delete/rebuild and event handling
  (Intel contributions)
- Resolves: rhbz#437169 rhbz#437173 rhbz#437177 rhbz#439088

* Fri Feb 08 2008 Ian Kent <ikent@redhat.com> - 1.0.0.rc15
- Bug 427550: dmraid segfaults on boot resulting in broken mirror
  - patch to fix SEGV when requesting activation of invalid raid set.
  the feature.
  Related: rhbz#427550

* Wed Feb 06 2008 Peter Jones <pjones@redhat.com> - 1.0.0.rc13-8
- Revert fix for 381501, since the RHEL kernel doesn't currently support
  the feature.
  Related: rhbz#381501

* Fri Jan 18 2008 Ian Kent <ikent@redhat.com> - 1.0.0.rc13-7
- fix incorrectly applied patch in spec file.
- Related: rhbz#236891

* Wed Nov 21 2007 Ian Kent <ikent@redhat.com> - 1.0.0.rc13-6
- Bug 381511: dmraid needs to generate UUIDs for lib device-mapper
  - add patch to set UUID.
  - add "DMRAID-" prefix to dmraid UUID string.
- Bug 381501: dmraid needs to activate device-mapper mirror resynchronization error handling
- Resolves: rhbz#381511 rhbz#381501

* Fri Nov 2 2007 Ian Kent <ikent@redhat.com> - 1.0.0.rc13-5
- Fix SEGV on "dmraid -r -E" (bz 236891)
- Resolves: rhbz#236891

* Mon Sep 10 2007 Heinz Mauelshagen <heinzm@redhat.com> - 1.0.0.rc13-4
- Adjusted %dist to rebuild
- Resolves: #211012

* Mon Sep 10 2007 Heinz Mauelshagen <heinzm@redhat.com> - 1.0.0.rc13-4
- Missed a bug with dm map names
- Resolves: #211012

* Tue Jun 26 2007 Heinz Mauelshagen <heinzm@redhat.com> - 1.0.0.rc13-3
- Fix dmraid map names
- Resolves: #211012
- Fix unaligned access messages
  Resolves: #210361, #211150
- Fix jmicron name parsing (bz#219058)

* Wed Nov  8 2006 Peter Jones <pjones@redhat.com> - 1.0.0.rc13-2
- We didn't change the API or ABI, so don't change the version number
  because it'll change the SONAME, which means we have to needlessly rebuild
  other packages.

* Wed Nov 08 2006 Heinz Mauelshagen <heinzm@redhat.com> - 1.0.0.rc14-1
- asr.c: fixed Adaptec HostRAID DDF1 metadata discovery (bz#211016)
- ddf1_crc.c: added crc() routine to avoid linking to zlib alltogether,
              because Ubuntu had problems with this
- dropped zlib build requirement

* Thu Oct 26 2006 Heinz Mauelshagen <heinzm@redhat.com> - 1.0.0.rc14-bz211016-1
- ddf1.c: get_size() fixed (bz#211016)
- ddf1_lib.c: ddf1_cr_off_maxpds_helper() fixed (bz#211016)

* Wed Oct 11 2006 Heinz Mauelshagen <heinzm@redhat.com> - 1.0.0.rc13-1
- metadata.c: fixed bug returning wrang unified RAID type (bz#210085)
- pdc.c: fixed magic number check

* Sun Oct 01 2006 Jesse Keating <jkeating@redhat.com> - 1.0.0.rc12-7
- rebuilt for unwind info generation, broken in gcc-4.1.1-21

* Fri Sep 22 2006 Heinz Mauelshagen <heinzm@redhat.com> - 1.0.0.rc12-1
- sil.c: quorate() OBO fix
- activate.c: handler() OBO fix
- added SNIA DDF1 support
- added reload functionality to devmapper.c
- added log_zero_sectors() to various metadata format handlers
- sil.[ch]: added JBOD support

* Fri Sep  1 2006 Peter Jones <pjones@redhat.com> - 1.0.0.rc11-4
- Require kpartx, so initscripts doesn't have to if you're not using dmraid

* Thu Aug 17 2006 Jesse Keating <jkeating@redhat.com> - 1.0.0.rc11-3
- Change Release to follow guidelines, and add dist tag.

* Thu Aug 17 2006 Peter Jones <pjones@redhat.com> - 1.0.0.rc11-FC6.3
- No more excludearch for s390/s390x

* Fri Jul 28 2006 Peter Jones <pjones@redhat.com> - 1.0.0.rc11-FC6.2
- Fix bounds checking on hpt37x error log
- Only build the .so, not the .a
- Fix asc.c duplication in makefile rule

* Wed Jul 12 2006 Jesse Keating <jkeating@redhat.com> - 1.0.0.rc11-FC6.1.1
- rebuild

* Fri Jul  7 2006 Heinz Mauelshagen <heinzm@redhat.com> - 1.0.0.rc11-FC6.1
- rebuilt for FC6 with dos partition discovery fix (#197573)

* Tue May 16 2006 Heinz Mauelshagen <heinzm@redhat.com> - 1.0.0.rc11-FC6
- rebuilt for FC6 with better tag

* Tue May 16 2006 Heinz Mauelshagen <heinzm@redhat.com> - 1.0.0.rc11-FC5_7.2
- rebuilt for FC5

* Tue May 16 2006 Heinz Mauelshagen <heinzm@redhat.com> - 1.0.0.rc11-FC5_7.1
- jm.c: checksum() calculation
- misc.c: support "%d" in p_fmt and fix segfault with wrong format identifier
- nv.c: size fix in setup_rd()
- activate.c:
        o striped devices could end on non-chunk boundaries
        o calc_region_size() calculated too small sizes causing large
          dirty logs in memory
- isw.c: set raid5 type to left asymmetric
- toollib.c: fixed 'No RAID...' message
- support selection of RAID5 allocation algorithm in metadata format handlers
- build

* Mon Mar 27 2006 Milan Broz <mbroz@redhat.com> - 1.0.0.rc10-FC5_6.2
- fixed /var/lock/dmraid in specfile (#168195)

* Fri Feb 17 2006 Heinz Mauelshagen <heinzm@redhat.com> - 1.0.0.rc10-FC5_6
- add doc/dmraid_design.txt to %doc (#181885)
- add --enable-libselinux --enable-libsepol to configure
- rebuilt

* Fri Feb 10 2006 Jesse Keating <jkeating@redhat.com> - 1.0.0.rc9-FC5_5.2
- bump again for double-long bug on ppc(64)

* Tue Feb 07 2006 Jesse Keating <jkeating@redhat.com> - 1.0.0.rc9-FC5_5.1
- rebuilt for new gcc4.1 snapshot and glibc changes

* Sun Jan 22 2006 Peter Jones <pjones@redhat.com> 1.0.0.rc9-FC5_5
- Add selinux build deps
- Don't set owner during make install

* Fri Dec  9 2005 Jesse Keating <jkeating@redhat.com> 1.0.0.rc9-FC5_4.1
- rebuilt

* Sun Dec  3 2005 Peter Jones <pjones@redhat.com> 1.0.0.rc9-FC5_4
- rebuild for device-mapper-1.02.02-2

* Fri Dec  2 2005 Peter Jones <pjones@redhat.com> 1.0.0.rc9-FC5_3
- rebuild for device-mapper-1.02.02-1

* Thu Nov 10 2005 Peter Jones <pjones@redhat.com> 1.0.0.rc9-FC5_2
- update to 1.0.0.rc9
- make "make install" do the right thing with the DSO
- eliminate duplicate definitions in the headers
- export more symbols in the DSO
- add api calls to retrieve dm tables
- fix DESTDIR for 'make install' 
- add api calls to identify degraded devices
- remove several arch excludes

* Sat Oct 15 2005 Florian La Roche <laroche@redhat.com>
- add -lselinux -lsepol for new device-mapper deps

* Fri May 20 2005 Heinz Mauelshagen <heinzm@redhat.com> 1.0.0.rc8-FC4_2
- specfile change to build static and dynamic binray into one package
- rebuilt

* Thu May 19 2005 Heinz Mauelshagen <heinzm@redhat.com> 1.0.0.rc8-FC4_1
- nv.c: fixed stripe size
- sil.c: avoid incarnation_no in name creation, because the Windows
         driver changes it every time
- added --ignorelocking option to avoid taking out locks in early boot
  where no read/write access to /var is given

* Wed Mar 16 2005 Elliot Lee <sopwith@redhat.com>
- rebuilt

* Tue Mar 15 2005 Heinz Mauelshagen <heinzm@redhat.com> 1.0.0.rc6.1-4_FC4
- VIA metadata format handler
- added RAID10 to lsi metadata format handler
- "dmraid -rD": file device size into {devicename}_{formatname}.size
- "dmraid -tay": pretty print multi-line tables ala "dmsetup table"
- "dmraid -l": display supported RAID levels + manual update
- _sil_read() used LOG_NOTICE rather than LOG_INFO in order to
  avoid messages about valid metadata areas being displayed
  during "dmraid -vay".
- isw, sil filed metadata offset on "-r -D" in sectors rather than in bytes.
- isw needed dev_sort() to sort RAID devices in sets correctly.
- pdc metadata format handler name creation. Lead to
  wrong RAID set grouping logic in some configurations.
- pdc RAID1 size calculation fixed (rc6.1)
- dos.c: partition table code fixes by Paul Moore
- _free_dev_pointers(): fixed potential OOB error
- hpt37x_check: deal with raid_disks = 1 in mirror sets
- pdc_check: status & 0x80 doesn't always show a failed device;
  removed that check for now. Status definitions needed.
- sil addition of RAID sets to global list of sets
- sil spare device memory leak
- group_set(): removal of RAID set in case of error
- hpt37x: handle total_secs > device size
- allow -p with -f
- enhanced error message by checking target type against list of
  registered target types

* Fri Jan 21 2005 Alasdair Kergon <agk@redhat.com> 1.0.0.rc5f-2
- Rebuild to pick up new libdevmapper.

* Fri Nov 26 2004 Heinz Mauelshagen <heinzm@redhat.com> 1.0.0.rc5f
- specfile cleanup

* Tue Aug 20 2004 Heinz Mauelshagen <heinzm@redhat.com> 1.0.0-rc4-pre1
- Removed make flag after fixing make.tmpl.in

* Tue Aug 18 2004 Heinz Mauelshagen <heinzm@redhat.com> 1.0.0-rc3
- Added make flag to prevent make 3.80 from looping infinitely

* Thu Jun 17 2004 Heinz Mauelshagen <heinzm@redhat.com> 1.0.0-pre1
- Created
