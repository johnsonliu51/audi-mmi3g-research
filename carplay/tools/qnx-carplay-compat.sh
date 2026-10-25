#!/bin/ksh
# QNX pdksh has print, but the reference MMI image has no printf utility.
# Deliberately narrow formatter for this package: %s, %%, \n, \t, \\.
# Values are always literal; neither print nor a format interprets their bytes.
# This is not a general replacement for POSIX printf.
cp_printf() {
    typeset CP_FMT CP_CHAR CP_NEXT CP_TEXT CP_NL CP_TAB
    [ "$#" -ge 1 ] || return 2
    CP_FMT="$1"
    shift
    CP_TEXT=
    CP_NL='
'
    CP_TAB='	'
    while [ -n "$CP_FMT" ]; do
        CP_CHAR="${CP_FMT%"${CP_FMT#?}"}"
        CP_FMT="${CP_FMT#?}"
        case "$CP_CHAR" in
            '%')
                CP_NEXT="${CP_FMT%"${CP_FMT#?}"}"
                CP_FMT="${CP_FMT#?}"
                case "$CP_NEXT" in
                    s) CP_TEXT="$CP_TEXT${1-}"; [ "$#" -eq 0 ] || shift ;;
                    '%') CP_TEXT="$CP_TEXT%" ;;
                    *) print -r -- 'compat_error=unsupported_format' >&2; return 2 ;;
                esac ;;
            '\')
                CP_NEXT="${CP_FMT%"${CP_FMT#?}"}"
                CP_FMT="${CP_FMT#?}"
                case "$CP_NEXT" in
                    n) CP_TEXT="$CP_TEXT$CP_NL" ;;
                    t) CP_TEXT="$CP_TEXT$CP_TAB" ;;
                    '\') CP_TEXT="$CP_TEXT\\" ;;
                    *) print -r -- 'compat_error=unsupported_escape' >&2; return 2 ;;
                esac ;;
            *) CP_TEXT="$CP_TEXT$CP_CHAR" ;;
        esac
    done
    [ "$#" -eq 0 ] || { print -r -- 'compat_error=extra_format_arguments' >&2; return 2; }
    print -rn -- "$CP_TEXT"
}
