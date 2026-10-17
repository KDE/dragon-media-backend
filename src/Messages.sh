#!/bin/sh
# SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
# SPDX-License-Identifier: CC0-1.0

$XGETTEXT `find . ../include -name \*.cpp -o -name \*.h -o -name \*.qml` -o $podir/dragonmediabackend.pot

