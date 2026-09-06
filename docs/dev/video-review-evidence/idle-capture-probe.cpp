#include "VideoBroadcaster.h"
#include "VideoSource.h"
#include <QGuiApplication>
#include <QSignalSpy>
#include <cstdio>
int main(int argc, char **argv) {
 QGuiApplication app(argc,argv);
 VideoBroadcaster broadcaster;
 broadcaster.setCodec(1);
 auto owned=std::make_unique<SyntheticVideoSource>(320,240);
 auto *source=owned.get();
 source->setChangeRatio(0);
 if (!broadcaster.start(std::move(owned))) return 2;
 QSignalSpy units(&broadcaster,&VideoBroadcaster::unitReady);
 source->pump(1);
 printf("Initial static frame units: %lld\n",static_cast<long long>(units.count()));
 units.clear();
 broadcaster.requestKeyframe();
 QCoreApplication::processEvents();
 printf("Recovery units without another capture callback: %lld\n",static_cast<long long>(units.count()));
 source->pump(2);
 printf("Recovery units once capture changes/callback resumes: %lld\n",static_cast<long long>(units.count()));
 return 0;
}
