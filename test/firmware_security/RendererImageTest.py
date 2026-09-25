"""Compile the actual renderer functions against a framebuffer-only host shell.

No firmware decoder/UI dependencies: this tests orientation, inversion, odd
bitmap widths and clipping using the production rotation/pixel/image bodies.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[2]
source=(ROOT/'lib/GfxRenderer/GfxRenderer.cpp').read_text()
def function(signature):
    start=source.index(signature)
    end=source.index('\n}',start)+2
    return source[start:end]

harness=r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
struct GfxRenderer {
  enum Orientation { Portrait, LandscapeClockwise, PortraitInverted, LandscapeCounterClockwise };
  Orientation orientation=Portrait;
  uint16_t panelWidth=24,panelHeight=16;
  uint32_t panelWidthBytes=3;
  mutable uint8_t storage[48];
  uint8_t* frameBuffer=storage;
  bool textClipActive_=false,_stripActive=false;
  int textClipLeft_=0,textClipRight_=0,textClipTop_=0,textClipBottom_=0,_stripY0=0,_stripRows=0;
  uint8_t* _stripBuf=nullptr;
  void drawPixel(int,int,bool) const;
  void drawImage(const uint8_t[],int,int,int,int) const;
  void drawImageInverted(const uint8_t[],int,int,int,int) const;
};
'''
for sig in ['static inline void rotateCoordinates(', 'void GfxRenderer::drawPixel(', 'void GfxRenderer::drawImage(', 'void GfxRenderer::drawImageInverted(']:
    harness+='\n'+function(sig)+'\n'
harness+=r'''
int main() {
  const uint8_t bitmap[]={0x80,0x80,0x55,0x00,0xf0,0x80}; // asymmetric 9x3
  for(int orientation=0;orientation<4;++orientation) for(int offset:{-2,0,7,22}) for(bool invert:{false,true}) {
    GfxRenderer r; r.orientation=static_cast<GfxRenderer::Orientation>(orientation);
    std::memset(r.storage,0xff,sizeof(r.storage));
    std::array<uint8_t,48> expected; expected.fill(0xff);
    for(int y=0;y<3;++y) for(int x=0;x<9;++x) {
      int lx=x+offset,ly=y+offset,px=0,py=0;
      switch(orientation) {
        case 0: px=ly;py=15-lx;break;
        case 1: px=23-lx;py=15-ly;break;
        case 2: px=23-ly;py=lx;break;
        case 3: px=lx;py=ly;break;
      }
      if(px<0||py<0||px>=24||py>=16)continue;
      bool white=(bitmap[y*2+x/8] & (0x80>>(x%8)))!=0;
      bool black=invert?white:!white;
      if(black)expected[py*3+px/8]&=~(0x80>>(px%8));
    }
    if(invert)r.drawImageInverted(bitmap,offset,offset,9,3);
    else r.drawImage(bitmap,offset,offset,9,3);
    assert(std::equal(expected.begin(),expected.end(),r.storage));
    r.drawImageInverted(nullptr,0,0,9,3);
    r.drawImageInverted(bitmap,0,0,0,3);
    assert(std::equal(expected.begin(),expected.end(),r.storage));
  }
}
'''
with tempfile.TemporaryDirectory() as directory:
    path=Path(directory)
    (path/'renderer.cpp').write_text(harness)
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-fsanitize=address,undefined',str(path/'renderer.cpp'),'-o',str(path/'renderer')],check=True)
    subprocess.run([str(path/'renderer')],check=True)
print('Bitmap orientation/inversion/clipping: 32 scenarios passed')
