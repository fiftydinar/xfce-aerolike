#!/bin/sh
TZ=$(curl -sf https://ipapi.co/timezone 2>/dev/null || curl -sf https://ipinfo.io/timezone 2>/dev/null)
[ -n "$TZ" ] && timedatectl set-timezone "$TZ" || true
