#ifndef INTERVALCHUNKSDISPLAY_H
#define INTERVALCHUNKSDISPLAY_H

#include <QProgressBar>

QT_BEGIN_NAMESPACE
class QPaintEvent;
QT_END_NAMESPACE

class IntervalChunksDisplay : public QProgressBar
{
    Q_OBJECT

public:
    explicit IntervalChunksDisplay(QWidget *parent);

    void incrementDownloadedChunks();
    void finish();
    void reset();
    void setEstimatedTotalChunks(int estimatedChunks);

private:
    int downloadedChunks; // downloaded chunks in current interval
    int totalChunks; // total chunks in a interval
};

#endif // INTERVALCHUNKSDISPLAY_H
