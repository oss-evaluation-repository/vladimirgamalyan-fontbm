#include <string>
#include <iostream>
#include <fstream>
#include <map>
#include <array>
#include <limits>
#include <algorithm>
#include <iomanip>
#include <SDL2/SDL.h>
#include <SDL2pp/SDL2pp.hh>
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#include "sdlSavePng/savepng.h"
#include "FontInfo.h"
#include "maxRectsBinPack/MaxRectsBinPack.h"
#include "Config.h"
#include "ProgramOptions.h"


namespace fs = boost::filesystem;

int getKerning(const SDL2pp::Font& font, uint32_t char0, uint32_t char1)
{
    if ((char0 > 0xFFFF) || (char1 > 0xFFFF))
        return 0;
    Uint16 ch0 = static_cast<Uint16>(char0);
    Uint16 ch1 = static_cast<Uint16>(char1);
    Uint16 text[ 3 ] = {ch0, ch1, 0};
    return font.GetSizeUNICODE(text).x - ( font.GetGlyphAdvance(ch0) + font.GetGlyphAdvance(ch1) );
}

void printGlyphData(const SDL2pp::Font& font, Uint16 ch)
{
    int minx, maxx, miny, maxy, advance;
    font.GetGlyphMetrics(ch, minx, maxx, miny, maxy, advance);
    std::cout << "minx=" << minx
        << ", maxx=" << maxx
        << ", miny=" << miny
        << ", maxy=" << maxy
        << ", advance: " << advance
        << std::endl;
}

struct GlyphInfo
{
    uint16_t page = 0;
    int x = 0;
    int y = 0;

    int getWidth() const
    {
        return maxx - minx;
    }

    int getHeight() const
    {
        return maxy - miny;
    }

    bool isEmpty() const
    {
        return ((getWidth() == 0) && (getHeight() == 0));
    }

    bool isInvalid() const
    {
        return ((!isEmpty()) && ((getWidth() <= 0) || (getHeight() <= 0)));
    }

    int minx = 0;
    int maxx = 0;
    int miny = 0;
    int maxy = 0;
    int advance = 0;
};

typedef std::map<uint32_t, GlyphInfo> Glyphs;

std::vector<rbp::RectSize> getSrcRects(const Glyphs &glyphs, int additionalWidth, int additionalHeight)
{
    std::vector<rbp::RectSize> result;
    for (auto& kv : glyphs)
    {
        const GlyphInfo& glyphInfo = kv.second;
        if (!glyphInfo.isEmpty())
        {
            rbp::RectSize rs;
            rs.width = glyphInfo.getWidth() + additionalHeight;
            rs.height = glyphInfo.getHeight() + additionalWidth;
            rs.tag = kv.first;
            result.push_back(rs);
        }
    }
    return result;
}

Glyphs getGlyphInfo(const SDL2pp::Font& font,
                                           const std::set<uint32_t>& codes,
                                           uint32_t maxTextureSizeX,
                                           uint32_t maxTextureSizeY)
{
    int fontAscent = font.GetAscent();

    Glyphs glyphs;

    for (auto& id : codes)
    {
        if ((id > 0xFFFF) || (!font.IsGlyphProvided(static_cast<Uint16>(id))))
        {
            std::cout << "warning: glyph " << id << " not found " << std::endl;
            continue;
        }

        GlyphInfo glyphInfo;
        font.GetGlyphMetrics(static_cast<Uint16>(id),
                             glyphInfo.minx,
                             glyphInfo.maxx,
                             glyphInfo.miny,
                             glyphInfo.maxy,
                             glyphInfo.advance);

        if (fontAscent < glyphInfo.maxy)
            throw std::runtime_error("invalid glyph (maxy > ascent)");

        if ( (glyphInfo.getWidth() > static_cast<int>(maxTextureSizeX)) || (glyphInfo.getHeight() > static_cast<int>(maxTextureSizeY)))
            throw std::runtime_error("no room for glyph");

        //TODO: add more checks for glyph.

        if (glyphInfo.isInvalid())
            throw std::runtime_error("invalid glyph (zero or negative width or height)");

        //TODO: emplace.
        glyphs[id] = glyphInfo;
    }

    return glyphs;
}

SDL_Color makeSdlColor(Config::Color c, uint8_t a = 255)
{
    return SDL_Color{c.r, c.g, c.b, a};
}

uint16_t arrangeGlyphs(Glyphs& glyphs, const Config& config)
{
    std::vector< rbp::RectSize > srcRects = getSrcRects(glyphs, config.spacing.hor + config.padding.left + config.padding.right,
                config.spacing.ver + config.padding.up + config.padding.down);

    rbp::MaxRectsBinPack mrbp;
    uint16_t pageCount = 0;
    for (;;)
    {
        //TODO: check negative dimension.
        mrbp.Init(config.textureSize.w - config.spacing.hor, config.textureSize.h - config.spacing.ver);

        std::vector<rbp::Rect> readyRects;
        mrbp.Insert( srcRects, readyRects, rbp::MaxRectsBinPack::RectBestAreaFit );
        if ( readyRects.empty() )
        {
            if ( !srcRects.empty() )
                throw std::runtime_error("can not fit glyphs to texture");
            break;
        }

        for ( auto r: readyRects )
        {
            glyphs[r.tag].x = r.x + config.spacing.hor;
            glyphs[r.tag].y = r.y + config.spacing.ver;
            glyphs[r.tag].page = pageCount;
        }

        pageCount++;
    }

    return pageCount;
}

int getDigitCount(uint16_t x)
{
    return (x < 10 ? 1 :
            (x < 100 ? 2 :
             (x < 1000 ? 3 :
              (x < 10000 ? 4 :
               5))));
}

int main(int argc, char** argv)
{
    try {
        const Config config = ProgramOptions::parseCommandLine(argc, argv);

        const fs::path dataFilePath = fs::absolute(fs::path(config.output + ".fnt"));
        const fs::path outputDirPath = dataFilePath.parent_path();
        const std::string outputName = dataFilePath.stem().string();

        //TODO: create directory only if there is no problem (exceptions), good place is right before write outputs.

        fs::create_directory(outputDirPath);

        if (!fs::is_regular_file(config.fontFile))
            throw std::runtime_error("font file not found");

        SDL2pp::SDLTTF ttf;
        SDL2pp::Font font(config.fontFile.generic_string(), config.fontSize);

        Glyphs glyphs = getGlyphInfo(font, config.chars, config.textureSize.w, config.textureSize.h);

        const uint16_t pageCount = arrangeGlyphs(glyphs, config);

        const int fontAscent = font.GetAscent();

        /////////////////////////////////////////////////////////////

        std::vector<std::string> pageNames;

        int pageNameDigits = getDigitCount(pageCount);

        for (size_t page = 0; page < pageCount; ++page)
        {
            //TODO: use real texture size instead max.
            SDL2pp::Surface outputSurface(0, config.textureSize.w, config.textureSize.h, 32,
                                          0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
            //std::cout << "outputSurface blend mode " << outputSurface.GetBlendMode() << std::endl;
            // SDL_BLENDMODE_BLEND = 1 (alpha).

            // If the color value contains an alpha component then the destination is simply
            // filled with that alpha information, no blending takes place.
            if (config.backgroundColor)
                outputSurface.FillRect(SDL2pp::NullOpt, config.backgroundColor->getUint32(255));
            else
                outputSurface.FillRect(SDL2pp::NullOpt, config.color.getUint32(0));

            for (auto kv: glyphs)
            {
                const GlyphInfo& glyph = kv.second;
                if (glyph.page != static_cast<int>(page))
                    continue;

                SDL2pp::Surface glyphSurface = font.RenderGlyph_Blended(kv.first, makeSdlColor(config.color));

                int x = glyph.x - glyph.minx;
                //if (glyph.minx < 0)
                  //  x = glyph.x;
                int y = glyph.y - (fontAscent - glyph.maxy);
                if (!glyph.isEmpty())
                {
                    x += config.padding.left;
                    y += config.padding.up;
                    SDL2pp::Rect dstRect(x, y, glyph.getWidth(), glyph.getHeight());
                    // Blit with alpha blending.
                    glyphSurface.Blit(SDL2pp::NullOpt, outputSurface, dstRect);
                }
            }

            std::stringstream ss;
            ss << outputName << "_" << std::setfill ('0') << std::setw(pageNameDigits) << page << ".png";
            std::string pageName = ss.str();
            pageNames.push_back(pageName);

            if (config.backgroundColor)
                outputSurface = outputSurface.Convert(SDL_PIXELFORMAT_RGB24);

            boost::filesystem::path texturePath = outputDirPath / boost::filesystem::path(pageName);
            SDL_SavePNG(outputSurface.Get(), texturePath.generic_string().c_str());
        }

        /////////////////////////////////////////////////////////////

        FontInfo f;

        f.info.face = font.GetFamilyName().value_or("unknown");
        f.info.size = config.fontSize;
        f.info.unicode = true;
        f.info.aa = 1;
        f.info.padding.up = static_cast<uint8_t>(config.padding.up);
        f.info.padding.right = static_cast<uint8_t>(config.padding.right);
        f.info.padding.down = static_cast<uint8_t>(config.padding.down);
        f.info.padding.left = static_cast<uint8_t>(config.padding.left);
        f.info.spacing.horizontal = static_cast<uint8_t>(config.spacing.hor);
        f.info.spacing.vertical = static_cast<uint8_t>(config.spacing.ver);

        f.common.lineHeight = static_cast<uint16_t>(font.GetLineSkip());
        f.common.base = static_cast<uint16_t>(font.GetAscent());
        f.common.scaleW = static_cast<uint16_t>(config.textureSize.w);
        f.common.scaleH = static_cast<uint16_t>(config.textureSize.h);
        f.common.pages = pageCount;

        for (size_t i = 0; i < pageCount; ++i )
            f.pages.push_back(pageNames.at(i));

        for (auto kv: glyphs)
        {
            //TODO: page = 0 for empty flyphs.
            const GlyphInfo &glyph = kv.second;
            FontInfo::Char c;
            c.id = kv.first;
            if (!glyph.isEmpty())
            {
                c.x = static_cast<uint16_t>(glyph.x);
                c.y = static_cast<uint16_t>(glyph.y);
                c.width = static_cast<uint16_t>(glyph.getWidth() + config.padding.left + config.padding.right);
                c.height = static_cast<uint16_t>(glyph.getHeight() + config.padding.up + config.padding.down);
                c.page = static_cast<uint8_t>(glyph.page);
                c.xoffset = static_cast<int16_t>(glyph.minx - config.padding.left);
                c.yoffset = static_cast<int16_t>(fontAscent - glyph.maxy - config.padding.up);
            }
            c.xadvance = static_cast<int16_t>(glyph.advance);
            c.chnl = 15;

            f.chars.push_back(c);
        }

        if (config.includeKerningPairs)
        {
            std::set<uint32_t> glyphCodes2(config.chars);
            for (auto& ch0 : config.chars)
            {
                for (auto& ch1 : glyphCodes2)
                {
                    int16_t k = static_cast<int16_t>(getKerning(font, ch0, ch1));
                    if (k)
                    {
                        FontInfo::Kerning kerning;
                        kerning.first = ch0;
                        kerning.second = ch1;
                        kerning.amount = k;
                        f.kernings.push_back(kerning);
                    }
                }
                glyphCodes2.erase(ch0);
            }
        }

        switch (config.dataFormat) {
            case Config::DataFormat::Xml:
                f.writeToXmlFile(dataFilePath.generic_string());
                break;
            case Config::DataFormat::Text:
                f.writeToTextFile(dataFilePath.generic_string());
                break;
            case Config::DataFormat::Bin:
                f.writeToBinFile(dataFilePath.generic_string());
                break;
            case Config::DataFormat::Json:
                f.writeToJsonFile(dataFilePath.generic_string());
                break;
        }

        return 0;

    } catch (std::exception& e) {
        std::cerr << "error: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "unknown error" << std::endl;
        return 1;
    }
}
