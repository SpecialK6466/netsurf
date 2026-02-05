#!/usr/bin/env bash

set -euo pipefail

# Host-side helper to scrape a small subset of a real website for offline
# browsing from the Dreamcast romdisk.
#
# Output goes to: frontends/dreamcast/res/sites/<name>/
# Offline entry point: file:///rd/sites/index.html

usage() {
	cat <<'EOF'
Usage:
  scrape_site.sh <name> <url> [--level N] [--quota SIZE]
  scrape_site.sh --list <sites.txt> [--level N] [--quota SIZE]

Options:
  --level N     Link-follow depth (wget --level). Default: 2
  --quota SIZE  Per-site download quota (wget --quota). Default: 2m
  --max-filesize SIZE
               Maximum size of any single downloaded file (wget --max-filesize).
               Default: 256k
  --aggressive  Allow cross-host downloads for page requisites (default)
  --conservative
               Restrict downloads to the origin host only

Notes:
  - This uses wget on the build host, not on Dreamcast.
  - By default it allows cross-host downloads for page requisites (CDN assets),
    but it is bounded by quota and an allowlist of common web asset extensions.
  - After scraping, it regenerates frontends/dreamcast/res/sites/index.html
EOF
}

LEVEL=2
QUOTA=2m

# Keep individual assets bounded; this is critical for Dreamcast RAM limits.
MAX_FILESIZE=256k

# If set, wipe existing output under frontends/dreamcast/res/sites before scraping.
CLEAN=0

# Default behavior:
# - More aggressive about external asset hosts (CDNs, etc.)
# - Still bounded by quota and an allowlist of common page asset types
AGGRESSIVE=1

# Keep the file set small and relevant for offline rendering on Dreamcast.
# (We intentionally reject large media and archive types.)
ACCEPT_EXTENSIONS="html,htm,css,js,mjs,json,xml,txt,png,jpg,jpeg,gif,svg,ico,woff,woff2,ttf,otf,eot,map"
REJECT_EXTENSIONS="webp,mp4,webm,mkv,avi,mov,mp3,ogg,wav,flac,zip,tar,gz,bz2,xz,7z,rar,iso,exe,dmg"

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
RES_DIR=$(cd "${SCRIPT_DIR}/../res" && pwd)
SITES_DIR="${RES_DIR}/sites"
STAMP_FILE="${SITES_DIR}/.stamp"

mkdir -p "${SITES_DIR}"

extract_host() {
	# Extract host from URL: scheme://host/...
	# Works for http(s) URLs.
	printf '%s' "$1" | awk -F/ '{print $3}'
}

ensure_site_index() {
	local index_tmp
	index_tmp=$(mktemp)

	{
		cat <<'HTML'
<!DOCTYPE html>
<html lang="en">
<head>
	<meta charset="utf-8" />
	<title>NetSurf Dreamcast - Offline Sites</title>
	<style>
		body { font-family: sans-serif; max-width: 760px; margin: 20px auto; padding: 0 12px; background: #f5f5f5; }
		h1 { color: #333; border-bottom: 2px solid #007acc; padding-bottom: 10px; }
		ul { list-style: none; padding: 0; }
		li { margin: 10px 0; padding: 10px; background: white; border-left: 4px solid #007acc; }
		a { font-size: 18px; text-decoration: none; color: #007acc; }
		a:hover { text-decoration: underline; }
		.small { color: #666; font-size: 14px; margin-top: 4px; }
	</style>
</head>
<body>
	<h1>Offline Sites (Romdisk)</h1>
	<p class="small">These pages were scraped on the host and packed into the Dreamcast romdisk.</p>
	<ul>
HTML

		for d in "${SITES_DIR}"/*; do
			[ -d "${d}" ] || continue
			local name
			name=$(basename "${d}")
			[ "${name}" = "." ] && continue
			[ "${name}" = ".." ] && continue
			# Skip hidden directories
			case "${name}" in
			\.*) continue ;;
			esac
			if [ -f "${d}/index.html" ]; then
				printf '\t\t<li><a href="%s/index.html">%s</a><div class="small">file:///rd/sites/%s/index.html</div></li>\n' "${name}" "${name}" "${name}"
			else
				printf '\t\t<li>%s<div class="small">(missing index.html)</div></li>\n' "${name}"
			fi
		done

		cat <<'HTML'
	</ul>
	<p><a href="file:///rd/en/welcome.html">&larr; Back to Welcome</a></p>
</body>
</html>
HTML
	} > "${index_tmp}"

	mkdir -p "${SITES_DIR}"
	mv "${index_tmp}" "${SITES_DIR}/index.html"
}

scrape_one() {
	local name=$1
	local url=$2

	if [ -z "${name}" ] || [ -z "${url}" ]; then
		usage >&2
		exit 2
	fi

	local host domains out_dir
	host=$(extract_host "${url}")
	if [ -z "${host}" ]; then
		echo "Could not parse host from URL: ${url}" >&2
		exit 2
	fi

	# Allow both www.foo and foo if applicable.
	if [[ "${host}" == www.* ]]; then
		domains="${host},${host#www.}"
	else
		domains="${host},www.${host}"
	fi

	out_dir="${SITES_DIR}/${name}"

	# Avoid unbounded growth across runs: clear the existing site directory.
	# (This directory is generated content under frontends/dreamcast/res/sites/.)
	rm -rf "${out_dir}"
	mkdir -p "${out_dir}"

	echo "SCRAPE ${name}: ${url}" >&2

	# wget will happily run forever if you let it. Keep it bounded.
	#
	# Aggressive mode:
	# - --span-hosts so CDN assets are pulled in
	# - Keep host directories to avoid path collisions between asset hosts
	# - --accept/--reject to keep the scrape reasonably small
	#
	# Conservative mode (AGGRESSIVE=0):
	# - Restrict to the main host and www variant
	# - Flatten host directories

	local wget_common
	wget_common=(
		--user-agent="NetSurf-Dreamcast-Offliner/1.0"
		--execute robots=off
		--directory-prefix="${out_dir}"
		--page-requisites
		--adjust-extension
		--convert-links
		--restrict-file-names=unix
		--no-parent
		--level="${LEVEL}"
		--quota="${QUOTA}"
		--max-filesize="${MAX_FILESIZE}"
		--timeout=10
		--tries=2
		--max-redirect=5
		--no-verbose
		--accept="${ACCEPT_EXTENSIONS}"
		--reject="${REJECT_EXTENSIONS}"
	)

	if [ "${AGGRESSIVE}" -ne 0 ]; then
		wget "${wget_common[@]}" --span-hosts "${url}" || true
	else
		wget "${wget_common[@]}" --no-host-directories --domains="${domains}" "${url}" || true
	fi

	# Prevent NetSurf from trying to fetch any missing *external* assets when
	# browsing the scraped copy from file://.
	#
	# We only neutralize URLs that are typically auto-fetched (src=, link href=,
	# CSS url(), @import). We do not rewrite normal <a href="http..."> links.
	sanitize_external_assets "${out_dir}"

	# Ensure a stable entry point (some sites won't naturally produce index.html)
	if [ ! -f "${out_dir}/index.html" ]; then
		local first_html
		first_html=$(find "${out_dir}" -maxdepth 2 -type f -name '*.html' | head -n 1 || true)
		if [ -n "${first_html}" ]; then
			local rel
			rel=${first_html#"${out_dir}/"}
			cat > "${out_dir}/index.html" <<EOF
<!DOCTYPE html>
<html lang="en">
<head>
	<meta charset="utf-8" />
	<meta http-equiv="refresh" content="0; url=${rel}" />
	<title>${name}</title>
</head>
<body>
	<p><a href="${rel}">Open ${name}</a></p>
</body>
</html>
EOF
		fi
	fi
}

sanitize_external_assets() {
	local out_dir=$1

	# HTML: neutralize external assets in tags that are typically auto-fetched.
	# Use data:, as an empty inline resource (no network request).
	find "${out_dir}" -type f \( -name '*.html' -o -name '*.htm' \) -print0 | \
		xargs -0 -r env LC_ALL=C perl -0777 -pi -e '
			s/(\bsrc\s*=\s*)(["\x27])https?:\/\/[^"\x27 >]+\2/$1$2."data:,".$2/ig;
			s/(\bposter\s*=\s*)(["\x27])https?:\/\/[^"\x27 >]+\2/$1$2."data:,".$2/ig;
			s/(\bsrcset\s*=\s*)(["\x27])([^"\x27]*?)\2/$1.$2.(do { my $v = $3; $v =~ s{https?:\/\/[^, \t\r\n]+}{data:,}ig; $v }).$2/ige;
			s/(<link\b[^>]*\bhref\s*=\s*)(["\x27])https?:\/\/[^"\x27 >]+\2/$1$2."data:,".$2/ig;
			'

	# CSS: neutralize external url() and @import.
	find "${out_dir}" -type f -name '*.css' -print0 | \
		xargs -0 -r env LC_ALL=C perl -0777 -pi -e '
			s/url\(\s*(["\x27]?)https?:\/\/[^)"\x27]+\1\s*\)/url("data:,")/ig;
			s/\@import\s+(url\()?\s*(["\x27]?)https?:\/\/[^;\)"\x27]+\2\s*(\))?\s*;/\@import url("data:,");/ig;
			'
}

parse_args() {
	local args=()
	while [ $# -gt 0 ]; do
		case "$1" in
			--clean)
				CLEAN=1; shift ;;
			--aggressive)
				AGGRESSIVE=1; shift ;;
			--conservative)
				AGGRESSIVE=0; shift ;;
			--level)
				LEVEL="$2"; shift 2 ;;
			--quota)
				QUOTA="$2"; shift 2 ;;
			--max-filesize)
				MAX_FILESIZE="$2"; shift 2 ;;
			--help|-h)
				usage; exit 0 ;;
			*)
				args+=("$1"); shift ;;
		esac
	done

	printf '%s\n' "${args[@]}"
}

main() {
	mapfile -t positional < <(parse_args "$@")

	if [ "${CLEAN}" -ne 0 ]; then
		# Generated content directory; safe to wipe.
		rm -rf "${SITES_DIR:?}"/*
	fi

	if [ ${#positional[@]} -ge 2 ] && [ "${positional[0]}" != "--list" ]; then
		scrape_one "${positional[0]}" "${positional[1]}"
		ensure_site_index
		touch "${STAMP_FILE}"
		exit 0
	fi

	if [ ${#positional[@]} -eq 2 ] && [ "${positional[0]}" = "--list" ]; then
		local list_file=${positional[1]}
		if [ ! -f "${list_file}" ]; then
			echo "Missing list file: ${list_file}" >&2
			exit 2
		fi

		while IFS= read -r line; do
			# Allow comments and blank lines.
			[ -z "${line}" ] && continue
			case "${line}" in
			\#*) continue ;;
			esac

			# Format: name <whitespace> url
			local name url
			name=$(printf '%s' "${line}" | awk '{print $1}')
			url=$(printf '%s' "${line}" | awk '{print $2}')
			[ -z "${name}" ] && continue
			[ -z "${url}" ] && continue
			scrape_one "${name}" "${url}"
		done < "${list_file}"

		ensure_site_index
		touch "${STAMP_FILE}"
		exit 0
	fi

	usage >&2
	exit 2
}

main "$@"
