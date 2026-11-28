KDIR=${1:-${KERNEL_DIR:-../linux}}
SRC=$KDIR/mm/khugepaged.c
UAPI=$KDIR/include/uapi/asm-generic/mman-common.h
EXPECT=${EXPECT_KERNEL:-7.1}
HINT='userfaultfd_armed|max_ptes_none|anon_vma|anon_rmap|is_khugepaged|hstart|hend|SCAN_EXCEED_NONE_PTE|SCAN_PAGE_ANON'
 
if [ ! -f "$SRC" ]; then
	echo "check-kernel: $SRC not found (set KERNEL_DIR=/path/to/linux)" >&2
	exit 2
fi
 
field() { sed -n "s/^$1 *= *//p" "$KDIR/Makefile" 2>/dev/null | head -n 1; }
ver="$(field VERSION).$(field PATCHLEVEL)"
echo "kernel source: $KDIR (version $ver$(field SUBLEVEL | sed 's/^/./'))"
case "$ver" in
"$EXPECT") ;;
*) echo "  warning: expected $EXPECT; re-audit the comments in src/policy/ if checks fail" ;;
esac
 
# Body of the top-level function whose definition starts in column 0.
body() {
	awk -v f="$1" '
		!on && $0 ~ "^[A-Za-z].*[ *]" f "\\(" { on = 1 }
		on { print }
		on && /^}/ { exit }' "$SRC"
}
 
oneline() { tr '\n\t' '  ' | tr -s ' '; }
 
fail=0
# check "description" "fn1 fn2 ..." 'single-line ERE'
# Passes if the regex matches the body of ANY of the listed functions.
check() {
	desc=$1 fns=$2 re=$3 found=
	for f in $fns; do
		b=$(body "$f")
		[ -z "$b" ] && continue
		found="$found $f"
		if printf '%s\n' "$b" | oneline | grep -Eq "$re"; then
			echo "  ok    $desc  [$f]"
			return 0
		fi
	done
	echo "  FAIL  $desc"
	if [ -z "$found" ]; then
		echo "        none of these functions exist in mm/khugepaged.c: $fns"
	else
		for f in $found; do
			echo "        $f() lines of interest (line numbers relative to function):"
			body "$f" | grep -nE "$HINT" | head -n 12 | sed 's/^/          /'
		done
	fi
	fail=1
}
 
check "uffd-armed VMA cannot take holes (unregister before collapse)" \
	"collapse_max_ptes_none __collapse_huge_page_isolate" \
	'userfaultfd_armed\(vma\)'
check "MADV_COLLAPSE ignores khugepaged max_ptes_none" \
	"collapse_policy_madvise __collapse_huge_page_isolate" \
	'pmd\.max_ptes_none = HPAGE_PMD_NR;|!cc->is_khugepaged \|\|'
check "collapse needs an anon_vma (zero-page-only region fails)" \
	"hugepage_vma_revalidate" \
	'anon_rmap|anon_vma'
check "errno: CGROUP_CHARGE_FAIL and EXCEED_NONE_PTE -> EBUSY" \
	madvise_collapse_errno \
	'case SCAN_CGROUP_CHARGE_FAIL: case SCAN_EXCEED_NONE_PTE: return -EBUSY;'
check "errno: ALLOC_HUGE_PAGE_FAIL -> ENOMEM" \
	madvise_collapse_errno \
	'case SCAN_ALLOC_HUGE_PAGE_FAIL: return -ENOMEM;'
check "errno: PAGE_COUNT / PAGE_LOCK -> EAGAIN" \
	madvise_collapse_errno \
	'case SCAN_PAGE_COUNT: case SCAN_PAGE_LOCK:[^;]*return -EAGAIN;'
check "errno: everything else -> EINVAL (not retried)" \
	madvise_collapse_errno \
	'default: return -EINVAL;'
 
if grep -Eq '#define[[:space:]]+MADV_COLLAPSE[[:space:]]+25([^0-9]|$)' "$UAPI" 2>/dev/null; then
	echo "  ok    MADV_COLLAPSE == 25 (fallback define in collapse.c)"
else
	echo "  FAIL  MADV_COLLAPSE == 25  [$UAPI]"
	fail=1
fi
 
# Informational only: bases are validated as 2 MiB aligned (promo_region_init),
# so a range without a whole aligned region never reaches the kernel.
if body madvise_collapse | oneline | grep -Eq 'if \(hstart >= hend\) return 0;'; then
	echo "  info  range without a whole aligned 2 MiB region: madvise returns 0 early"
else
	echo "  info  no early return for ranges without a whole aligned 2 MiB region (not relied on)"
fi
 
if [ "$fail" -eq 0 ]; then
	echo "check-kernel: all assumptions hold"
else
	echo "check-kernel: assumptions changed; update src/policy/collapse.c and policy.c" >&2
fi
exit "$fail"
 