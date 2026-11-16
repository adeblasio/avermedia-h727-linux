#!/bin/sh
# One-time setup so tests can run without a password prompt.
# Run with: sudo sh install-devtools.sh
# Remove again with: sudo sh install-devtools.sh --remove
set -eu

USER_NAME="${SUDO_USER:-$(logname)}"
SRC="$(cd "$(dirname "$0")" && pwd)"
RULE=/etc/sudoers.d/avertm62-dev

if [ "${1:-}" = "--remove" ]; then
	rm -f /usr/local/sbin/avertm62-load /usr/local/sbin/tm62reg "$RULE"
	echo "removed"
	exit 0
fi

LOADER="$(mktemp)"
sed "s|@SRCDIR@|$SRC|" "$SRC/avertm62-load" > "$LOADER"
install -o root -g root -m 0755 "$LOADER" /usr/local/sbin/avertm62-load
rm -f "$LOADER"
install -o root -g root -m 0755 "$SRC/tm62reg.py" /usr/local/sbin/tm62reg

TMP="$(mktemp)"
cat > "$TMP" <<EOF
# Passwordless access for H727 driver development (remove with install-devtools.sh --remove)
$USER_NAME ALL=(root) NOPASSWD: /usr/local/sbin/avertm62-load, /usr/local/sbin/avertm62-load *, /usr/sbin/rmmod avertm62, /usr/local/sbin/tm62reg *
EOF
visudo -cf "$TMP"
install -o root -g root -m 0440 "$TMP" "$RULE"
rm -f "$TMP"
echo "installed: /usr/local/sbin/avertm62-load, /usr/local/sbin/tm62reg, $RULE"
