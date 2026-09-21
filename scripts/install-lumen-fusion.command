#!/usr/bin/env bash
set -euo pipefail

package_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
install_parent="${HOME}/.local/share"
install_dir="${install_parent}/lumen-fusion"
mkdir -p "${install_parent}" "${HOME}/.local/bin"

if [[ -d "${install_dir}" ]]; then
  installed_root="$(cd "${install_dir}" && pwd -P)"
else
  installed_root=""
fi

if [[ "${package_root}" != "${installed_root}" ]]; then
  staging="$(mktemp -d "${install_parent}/.lumen-fusion-new.XXXXXX")"
  old_runtime=""
  cleanup() {
    [[ ! -d "${staging}" ]] || rm -rf "${staging}"
    if [[ -n "${old_runtime}" && -d "${old_runtime}" && ! -d "${install_dir}" ]]; then
      mv "${old_runtime}" "${install_dir}"
    fi
  }
  trap cleanup EXIT

  cp -R "${package_root}/." "${staging}/"
  if [[ -d "${install_dir}" ]]; then
    old_runtime="$(mktemp -d "${install_parent}/.lumen-fusion-old.XXXXXX")"
    rmdir "${old_runtime}"
    mv "${install_dir}" "${old_runtime}"
  fi
  mv "${staging}" "${install_dir}"
  if [[ -n "${old_runtime}" ]]; then
    rm -rf "${old_runtime}"
    old_runtime=""
  fi
  trap - EXIT
fi

cat > "${HOME}/.local/bin/lumen-fusion" <<'WRAPPER'
#!/usr/bin/env bash
set -euo pipefail

cd "${HOME}/.local/share/lumen-fusion"
exec ./bin/lumina "$@"
WRAPPER
chmod +x "${HOME}/.local/bin/lumen-fusion"
printf 'Installed. Run %s/.local/bin/lumen-fusion\n' "${HOME}"
