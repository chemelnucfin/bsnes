//single-threaded
auto PPU::Line::cacheBackground(PPU::IO::Background& bg) -> void {
  if(y == 1) {
    bg.mosaicCounter = ppu.io.mosaicSize + 1;
    bg.mosaicOffset = 1;
  } else if(--bg.mosaicCounter == 0) {
    bg.mosaicCounter = ppu.io.mosaicSize + 1;
    bg.mosaicOffset += ppu.io.mosaicSize + 1;
  }
}

//parallelized
auto PPU::Line::renderBackground(PPU::IO::Background& self, uint source) -> void {
  if(!self.aboveEnable && !self.belowEnable) return;
  if(self.tileMode == TileMode::Mode7) return renderMode7(self, source);
  if(self.tileMode == TileMode::Inactive) return;

  bool windowAbove[256];
  bool windowBelow[256];
  renderWindow(self.window, self.window.aboveEnable, windowAbove);
  renderWindow(self.window, self.window.belowEnable, windowBelow);

  bool hires = io.bgMode == 5 || io.bgMode == 6;
  bool offsetPerTileMode = io.bgMode == 2 || io.bgMode == 4 || io.bgMode == 6;
  bool directColorMode = io.col.directColor && source == Source::BG1 && (io.bgMode == 3 || io.bgMode == 4);
  int width = 256 << hires;

  uint tileHeight = 3 + self.tileSize;
  uint tileWidth = !hires ? tileHeight : 4;
  uint tileMask = 0x0fff >> self.tileMode;
  uint tiledataIndex = self.tiledataAddress >> 3 + self.tileMode;

  uint paletteBase = io.bgMode == 0 ? source << 5 : 0;
  uint paletteShift = 2 << self.tileMode;

  uint hscroll = self.hoffset;
  uint vscroll = self.voffset;
  uint hmask = (width << self.tileSize << !!(self.screenSize & 1)) - 1;
  uint vmask = (width << self.tileSize << !!(self.screenSize & 2)) - 1;

  uint y = self.mosaicEnable ? self.mosaicOffset : this->y;
  if(hires) {
    hscroll <<= 1;
    if(io.interlace) y = y << 1 | field();
  }

  uint mosaicCounter = 1;
  uint mosaicPalette = 0;
  uint mosaicPriority = 0;
  uint mosaicColor = 0;

  int x = 0 - (hscroll & 7);
  while(x < width) {
    uint hoffset = x + hscroll;
    uint voffset = y + vscroll;
    if(offsetPerTileMode) {
      uint validBit = 0x2000 << source;
      uint offsetX = x + (hscroll & 7);
      if(offsetX >= 8) {  //first column is exempt
        uint hlookup = getTile(io.bg3, (offsetX - 8) + (io.bg3.hoffset & ~7), io.bg3.voffset + 0);
        if(io.bgMode == 4) {
          if(hlookup & validBit) {
            if(!(hlookup & 0x8000)) {
              hoffset = offsetX + (hlookup & ~7);
            } else {
              voffset = y + hlookup;
            }
          }
        } else {
          uint vlookup = getTile(io.bg3, (offsetX - 8) + (io.bg3.hoffset & ~7), io.bg3.voffset + 8);
          if(hlookup & validBit) {
            hoffset = offsetX + (hlookup & ~7);
          }
          if(vlookup & validBit) {
            voffset = y + vlookup;
          }
        }
      }
    }
    hoffset &= hmask;
    voffset &= vmask;

    uint tileNumber = getTile(self, hoffset, voffset);
    uint mirrorY = tileNumber & 0x8000 ? 7 : 0;
    uint mirrorX = tileNumber & 0x4000 ? 7 : 0;
    uint tilePriority = self.priority[bool(tileNumber & 0x2000)];
    uint paletteNumber = tileNumber >> 10 & 7;
    uint paletteIndex = paletteBase + (paletteNumber << paletteShift) & 0xff;

    if(tileWidth  == 4 && (bool(hoffset & 8) ^ bool(mirrorX))) tileNumber +=  1;
    if(tileHeight == 4 && (bool(voffset & 8) ^ bool(mirrorY))) tileNumber += 16;
    tileNumber = (tileNumber & 0x03ff) + tiledataIndex & tileMask;

    auto tiledata = ppu.tilecache[self.tileMode] + (tileNumber << 6);
    tiledata += (voffset & 7 ^ mirrorY) << 3;

    for(uint tileX = 0; tileX < 8; tileX++, x++) {
      if(x & width) continue;  //x < 0 || x >= width
      if(!self.mosaicEnable || --mosaicCounter == 0) {
        mosaicCounter = 1 + io.mosaicSize;
        mosaicPalette = tiledata[tileX ^ mirrorX];
        mosaicPriority = tilePriority;
        if(directColorMode) {
          mosaicColor = directColor(paletteNumber, mosaicPalette);
        } else {
          mosaicColor = cgram[paletteIndex + mosaicPalette];
        }
      }
      if(!mosaicPalette) continue;

      if(!hires) {
        if(self.aboveEnable && !windowAbove[x]) plotAbove(x, source, mosaicPriority, mosaicColor);
        if(self.belowEnable && !windowBelow[x]) plotBelow(x, source, mosaicPriority, mosaicColor);
      } else {
        uint X = x >> 1;
        if(!ppu.hd()) {
          if(x & 1) {
            if(self.aboveEnable && !windowAbove[X]) plotAbove(X, source, mosaicPriority, mosaicColor);
          } else {
            if(self.belowEnable && !windowBelow[X]) plotBelow(X, source, mosaicPriority, mosaicColor);
          }
        } else {
          if(self.aboveEnable && !windowAbove[X]) plotHD(above, X, source, mosaicPriority, mosaicColor, true, x & 1);
          if(self.belowEnable && !windowBelow[X]) plotHD(below, X, source, mosaicPriority, mosaicColor, true, x & 1);
        }
      }
    }
  }
}

auto PPU::Line::getTile(PPU::IO::Background& self, uint hoffset, uint voffset) -> uint {
  bool hires = io.bgMode == 5 || io.bgMode == 6;
  uint tileHeight = 3 + self.tileSize;
  uint tileWidth = !hires ? tileHeight : 4;
  uint screenX = self.screenSize & 1 ? 32 << 5 : 0;
  uint screenY = self.screenSize & 2 ? 32 << 5 + (self.screenSize & 1) : 0;
  uint tileX = hoffset >> tileWidth;
  uint tileY = voffset >> tileHeight;
  uint offset = (tileY & 0x1f) << 5 | (tileX & 0x1f);
  if(tileX & 0x20) offset += screenX;
  if(tileY & 0x20) offset += screenY;
  return ppu.vram[self.screenAddress + offset & 0x7fff];
}
