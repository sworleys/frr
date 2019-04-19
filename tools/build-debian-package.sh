#!/bin/sh
#
# Written by Daniil Baturin, 2018
# This file is public domain

# NOTES
# add debian10 next to debian9 in debianpkg/rules
# You may have to set EXTRA_VERSION and FRR_VERSION
# The source for cumulus FRR will be under the debian-build directory

#
# Command wrapper function to deliniate log output and error check.
EXTRA_VERSION="+cl4u1"
FRR_VERSION="6.1"
fxnRunCmd()
{
    local result

    # Echoed text gets a different box
    if [ "$1" = "echo" ];then
	echo ""
	echo " _______________________________________________________________________________"
	echo "|"
	echo -n "| "
	$@
	echo "|_______________________________________________________________________________"
	echo ""
    else
	echo ""
	echo "#################################################################################"
	echo "# - Running: $@"
	echo "#################################################################################"
	echo ""
	# run everything passed
	$@
    fi

    # Store the result to echo it to the user
    result="$?"
    if [ "$result" != "0" ];then
	echo "ERROR! Command [ $@ ] returned error: $result"
	exit 1
    fi
}

#
# If the first argument is clean, wipe out everything not under source control.
if [ "$1" = "clean" ];then
    fxnRunCmd git clean -dx --force
    exit $?
fi

# Q - add packages to build libyang too?
BUILD_DEPENDS="build-essential \
 gawk \
 automake \
 autoconf \
 libtool \
 texinfo \
 binutils \
 libreadline6-dev \
 libjson-c-dev \
 libjson-c3 \
 libpam0g-dev \
 debhelper \
 dh-autoreconf \
 dejagnu \
 libsystemd-dev \
 dh-systemd \
 bison \
 flex \
 libc-ares-dev \
 clang \
 python-pytest \
 python-ipaddr \
 libsnmp-dev \
 libpython-dev \
 pkg-config \
 libsystemd-dev \
 chrpath \
 libcap-dev \
 libyang0.16 \
 libyang-dev \
 python-sphinx"

if [ "$1" = "deps" ];then
    fxnRunCmd echo "Installing build dependency packages."
    sudo apt-get update
    sudo apt-get install -y --no-install-recommends $BUILD_DEPENDS
    # stash this in the home dir until it makes it into the repo.
    # This copy will get trashed on the next clean
    if [ 1 = 0 ];then
	cp -r ~/libyang-debs .

	# this'll throw dependency errors because dpkg won't resolve them
	sudo dpkg -i ./libyang-debs/*deb
	# ...but fix-broken will...
	sudo apt-get -y --fix-broken install
    fi
    fxnRunCmd echo "Done"
    exit 1
fi
git diff-index --quiet HEAD || echo "Warning: git working directory is not clean!"

# Set the defaults
if [ "$EXTRA_VERSION" = "" ]; then
    EXTRA_VERSION="-MyDebPkgVersion"
fi

if [ "$WANT_SNMP" = "" ]; then
    WANT_SNMP=0
fi

WANT_CUMULUS_MODE=1
# Q - what is Cumulus Mode?
if [ "$WANT_CUMULUS_MODE" = "" ]; then
    WANT_CUMULUS_MODE=0
    CONFIGURE_ARGS="--with-pkg-extra-version=$EXTRA_VERSION    "
else
    CONFIGURE_ARGS="  --build=x86_64-linux-gnu \
                      --enable-configfile-mask=0640 \
                      --enable-cumulus=yes \
                      --enable-datacenter=yes \
                      --enable-exampledir=/usr/share/doc/frr/examples/ \
                      --enable-group=frr \
                      --enable-logfile-mask=0640 \
                      --enable-multipath=256 \
                      --enable-ospfapi=yes \
                      --enable-rtadv \
                      --enable-snmp \
                      --enable-systemd=yes \
                      --enable-user=frr \
                      --enable-vty-group=frrvty \
                      --enable-vtysh=yes \
                      --enable-werror \
                      --includedir=\${prefix}/include \
                      --infodir=\${prefix}/share/info \
                      --libexecdir=\${prefix}/lib/frr \
                      --localstatedir=/var/run/frr \
                      --mandir=\${prefix}/share/man \
                      --prefix=/usr \
                      --sbindir=/usr/lib/frr \
                      --sysconfdir=/etc \
                      --sysconfdir=/etc/frr \
                      --with-libpam \
                      --with-vtysh-pager=cat"
fi

fxnRunCmd echo "Preparing the build. Run '$0 clean' to reset everything."


fxnRunCmd ./bootstrap.sh

fxnRunCmd ./configure $CONFIGURE_ARGS


fxnRunCmd make dist

fxnRunCmd echo "Preparing Debian source package"

# Copy this reference directory of things needed for a Debian dpkg
# build...and name it 'debian' so dpkg-buildpackage can find it.
# A 'mv' will keep git from recognizing it when cleanup runs,
# and it will get deleted.
fxnRunCmd cp -ar debianpkg debian

# this will generate backports tar files for different
# debian/ubuntu releases

echo "Generating Debian build configuration for known releases."
fxnRunCmd make -f debian/rules backports


buildDir=$(pwd)
DEBIAN_BUILD_DIR="debian-builds"
fxnRunCmd echo "Unpacking the source to $DEBIAN_BUILD_DIR/"

fxnRunCmd mkdir $DEBIAN_BUILD_DIR

cd $DEBIAN_BUILD_DIR

BUILT_DEBS_DIR="$DEBIAN_BUILD_DIR"

fxnRunCmd echo "Copying orig source upstream tarball FRR tar.gz to $PWD"
# Keep a copy here for Debian build tools
fxnRunCmd mv ../frr_${FRR_VERSION}${EXTRA_VERSION}.orig.tar.gz .

#EX tar -xf ../frr_6.1+cl4u1.orig.tar.gz

fxnRunCmd tar xf ./frr_${FRR_VERSION}${EXTRA_VERSION}.orig.tar.gz
#EX frr-6.1+cl4u1

cd frr*
fxnRunCmd echo "Extracted orig upstream source FRR tar.gz to create Debian build directory: $PWD"

. /etc/os-release

grep -q "buster" /etc/os-release
if [ "$?" = "0" ];then
    # buster doesn't have a version_id at the moment
    VERSION_ID=10
fi

echo "Selecting Debian specific FRR archive with:"
echo "EXTRA_VERSION   = $EXTRA_VERSION"
echo "ID              = $ID"
echo "VERSION_ID      = $VERSION_ID"
echo "Match pattern   :  frr_*\${EXTRA_VERSION}\${ID}\${VERSION_ID}*.debian.tar.xz "
echo "Pattern becomes :  frr_*${EXTRA_VERSION}${ID}${VERSION_ID}*.debian.tar.xz "

fxnRunCmd echo "Extracting Debian specific code into the build directory at $PWD"
fxnRunCmd tar xf ../../frr_${FRR_VERSION}*${EXTRA_VERSION}-1~${ID}${VERSION_ID}*.debian.tar.xz

fxnRunCmd echo "Building the Debian package in $PWD"
fxnRunCmd dpkg-buildpackage -b -uc -us

fxnRunCmd echo "Built .debs are in $BUILT_DEBS_DIR"
ls -lrt $BUILT_DEBS_DIR
echo ""
fxnRunCmd echo "Done."

#debuild --no-lintian --set-envvar=WANT_SNMP=$WANT_SNMP --set-envvar=WANT_CUMULUS_MODE=$WANT_CUMULUS_MODE -b -uc -us
