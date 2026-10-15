#ifndef CHORDSPROGRESSIONPARSER_H
#define CHORDSPROGRESSIONPARSER_H

#include "ChordProgression.h"

QT_BEGIN_NAMESPACE
class QString;
QT_END_NAMESPACE

class ChordsProgressionParser
{
public:
    virtual ChordProgression parse(const QString &string) = 0;
    virtual ~ChordsProgressionParser(){}
    virtual bool containsProgression(const QString &string) = 0;
};

#endif // CHORDSPROGRESSIONPARSER_H
