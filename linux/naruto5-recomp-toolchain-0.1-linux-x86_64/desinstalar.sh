#!/bin/sh
# Remove a build e o cache gerados pela instalacao. Nao toca na ISO nem nos saves.
cd "$(dirname "$0")" || exit 1
printf "Remover build/, generated/, games/ e build/tools? [s/N] "
read -r answer
case "$answer" in
    s|S|y|Y) ;;
    *) echo "cancelado"; exit 0 ;;
esac
rm -rf build generated games build/tools .analysis-key
echo "removido. Rode ./instalar.sh para gerar de novo."
