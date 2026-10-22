#!/usr/bin/env bash

set -e

cd "$(dirname "$0")"

rm -rf ramdisk0
cp -r _ramdisk0 ramdisk0

EXES_EXAMPLE=$(find ../app/example -mindepth 2 -maxdepth 2 -name '*.exe' -type f 2>/dev/null || true)
EXES_TEST=$(find ../app/test -mindepth 2 -maxdepth 2 -name '*.exe' -type f 2>/dev/null || true)
if [[ "${_LIBC:-musl}" == musl ]] ; then
	EXES_TEST="$EXES_TEST $(find ../app/test/musl -mindepth 2 -maxdepth 2 -name '*.exe' -type f 2>/dev/null || true)"
fi

NCURSES_DIR=../app/usr/ncurses-6.6-install
NCURSES_PROGRAMS_DIR=../app/usr/ncurses
OKSH_DIR=../app/usr/oksh-7.8-install
TOYBOX_DIR=../app/usr/toybox-0.8.9-install
SBASE_DIR=../app/usr/sbase-install
UBASE_DIR=../app/usr/ubase-install
VIM_DIR=../app/usr/vim-9.2.0428-install
NANO_DIR=../app/usr/nano-9.0-install
LESS_DIR=../app/usr/less-692-install
PYTHON_DIR=../app/usr/Python-3.13.13-install-mario

print_deps()
{
	printf '%s\n' \
		rd-make.exe rd-explorer.exe \
		$EXES_EXAMPLE \
		$EXES_TEST \
		$(find "$NCURSES_DIR"/share/terminfo/ -type f 2>/dev/null || true) \
		$(find "$NCURSES_PROGRAMS_DIR" -name '*.exe' 2>/dev/null || true) \
		"$OKSH_DIR/bin/oksh" \
		"$TOYBOX_DIR/bin/toybox" \
		"$SBASE_DIR/bin/sbase-box" \
		"$UBASE_DIR/bin/ubase-box" \
		"$VIM_DIR/bin/vim" \
		"$NANO_DIR/bin/nano" \
		"$LESS_DIR/bin/less" \
		"$PYTHON_DIR/bin/python3.13" \
		$(find _ramdisk0 -type f 2>/dev/null || true) \
		device_list
}

if [[ "${1:-}" == --deps ]] ; then
	print_deps
	exit 0
fi

mkdir -p ramdisk0/{bin/example,bin/test,mnt,proc,root/work,sys/kernel/debug,usr/share/terminfo}
cp -u $EXES_EXAMPLE ramdisk0/bin/example
cp -u $EXES_TEST ramdisk0/bin/test

cp -u tools/rd-make.c ramdisk0/root/work/

# ncurses
mkdir -p ramdisk0/usr/share/terminfo/{a,l,v,x}
cp -u $NCURSES_DIR/share/terminfo/a/ansi ramdisk0/usr/share/terminfo/a
cp -u $NCURSES_DIR/share/terminfo/l/linux ramdisk0/usr/share/terminfo/l
cp -u $NCURSES_DIR/share/terminfo/v/{vt100,vt102} ramdisk0/usr/share/terminfo/v
cp -u $NCURSES_DIR/share/terminfo/x/xterm ramdisk0/usr/share/terminfo/x

cp -u $NCURSES_DIR/bin/* ramdisk0/bin

# oksh
cp -u $OKSH_DIR/bin/oksh ramdisk0/bin

# ncurses programs
mkdir -p ramdisk0/bin/ncurses
cp -u $NCURSES_PROGRAMS_DIR/*.exe ramdisk0/bin/ncurses

#	toybox
mkdir -p ramdisk0/bin/toybox
cp -u --no-dereference $TOYBOX_DIR/bin/* ramdisk0/bin/toybox

#	sbase
mkdir -p ramdisk0/bin/sbase
cp -u --no-dereference $SBASE_DIR/bin/* ramdisk0/bin/sbase

#	ubase
mkdir -p ramdisk0/bin/ubase
cp -u --no-dereference $UBASE_DIR/bin/* ramdisk0/bin/ubase

#	vim
mkdir -p ramdisk0/usr/share
cp -u --no-dereference $VIM_DIR/bin/* ramdisk0/bin/
cp -u -r $VIM_DIR/share/vim ramdisk0/usr/share

rm -f ramdisk0/bin/vimtutor
rm -f vim ramdisk0/bin/vi && ln -s vim ramdisk0/bin/vi

#	nano
mkdir -p ramdisk0/usr/share
cp -u --no-dereference $NANO_DIR/bin/* ramdisk0/bin/
cp -u -r $NANO_DIR/share/nano ramdisk0/usr/share

#	less
cp -u --no-dereference $LESS_DIR/bin/* ramdisk0/bin/

#	python
mkdir -p ramdisk0/usr/python/{bin,lib}
cp -u --no-dereference $PYTHON_DIR/bin/{python3,python3.13} ramdisk0/usr/python/bin/
cp -r -u --no-dereference $PYTHON_DIR/lib/python3.13 ramdisk0/usr/python/lib/

strip --strip-all ramdisk0/usr/python/bin/python3.13

rm -f ramdisk0/bin/python && ln -s /usr/python/bin/python3 ramdisk0/bin/python

for x in $(find ramdisk0/usr/python/ -name __pycache__) ; do \
	rm -r "$x" ; \
done

# strip executables
chmod +w ramdisk0/bin/toybox/toybox
chmod -x ramdisk0/bin/ncursesw_g6-config
for x in $(find ramdisk0/bin/ -type f -executable) ; do \
	strip --strip-all $x ; \
done

./rd-make.exe --dir=ramdisk0 --rd=rd0 --device-list=device_list --nr-block=81920
