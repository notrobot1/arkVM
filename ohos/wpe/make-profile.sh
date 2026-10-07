#!/bin/bash
# Выпуск разрешения (.p7b) для нашего приложения.
#
# Разрешение привязано к имени пакета и задаёт уровень доверия. Берём
# образец из средств подписи, правим три поля и подписываем тем же ключом,
# которым подписаны разрешения в дереве.
#
#   bash make-profile.sh com.arkvm.browser browser.p7b

set -euo pipefail

BUNDLE="${1:-com.arkvm.browser}"
OUT="${2:-$(dirname "$0")/signature/${BUNDLE##*.}.p7b}"

T=/mnt/ohos/OpenHarmony-6.1-Release/developtools/hapsigner/dist
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

command -v java >/dev/null || { echo "нужна java"; exit 1; }
[ -f "$T/hap-sign-tool.jar" ] || { echo "нет $T/hap-sign-tool.jar"; exit 1; }

# Образец. Правим имя пакета, уровень доверия и срок:
#   apl=system_core + app-feature=hos_system_app — тот же уровень, что у
#   файлового управляющего, он даёт право на системные разрешения;
#   срок в образце давно истёк, ставим далёкий.
python3 - "$T/UnsgnedReleasedProfileTemplate.json" "$TMP/profile.json" "$BUNDLE" <<'EOF'
import json, sys, uuid
src, dst, bundle = sys.argv[1], sys.argv[2], sys.argv[3]
p = json.load(open(src))
p["bundle-info"]["bundle-name"] = bundle
p["bundle-info"]["apl"] = "system_core"
p["bundle-info"]["app-feature"] = "hos_system_app"
p["uuid"] = str(uuid.uuid4())
p["validity"] = {"not-before": 1594865258, "not-after": 3250000000}
json.dump(p, open(dst, "w"), indent=4, ensure_ascii=False)
print("имя пакета:", bundle)
EOF

mkdir -p "$(dirname "$OUT")"
java -jar "$T/hap-sign-tool.jar" sign-profile \
    -keyAlias "openharmony application profile release" \
    -signAlg SHA256withECDSA \
    -mode localSign \
    -profileCertFile "$T/OpenHarmonyProfileRelease.pem" \
    -inFile "$TMP/profile.json" \
    -keystoreFile "$T/OpenHarmony.p12" \
    -outFile "$OUT" \
    -keyPwd 123456 \
    -keystorePwd 123456

echo "выпущено: $OUT"
strings -a "$OUT" | grep -oE '"bundle-name"[^,]*|"apl"[^,]*|"app-feature"[^,]*' | head
