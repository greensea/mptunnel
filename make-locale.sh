#!/bin/sh

xgettext -o mptunnel.pot -k_ --from-code=utf-8 *.c


cd locale

for i in zh_CN
do
    if [ -e $i.po ]
    then
        msgmerge -U $i.po ../mptunnel.pot
        mkdir -p $i/LC_MESSAGES
        msgfmt -o $i/LC_MESSAGES/mptunnel.mo $i.po
    else
        msginit --locale=$i -i ../mptunnel.pot -o $i.po --no-translator
    fi
done

cd ../

exit 0
