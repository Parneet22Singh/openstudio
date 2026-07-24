#pragma once

#include <JuceHeader.h>

/** Pitch / drum naming helpers for the editor UI. */
namespace NoteNames
{
    inline juce::String pitch (int n)
    {
        static const char* names[] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
        return juce::String (names[((n % 12) + 12) % 12]) + juce::String (n / 12 - 1);
    }

    /** General MIDI percussion names (channel-10 drum map). Empty if not a drum note. */
    inline juce::String gmDrum (int n)
    {
        switch (n)
        {
            case 35: return "Acoustic Kick";   case 36: return "Kick";
            case 37: return "Side Stick";      case 38: return "Snare";
            case 39: return "Hand Clap";       case 40: return "Electric Snare";
            case 41: return "Low Floor Tom";   case 42: return "Closed Hi-Hat";
            case 43: return "High Floor Tom";  case 44: return "Pedal Hi-Hat";
            case 45: return "Low Tom";         case 46: return "Open Hi-Hat";
            case 47: return "Low-Mid Tom";     case 48: return "Hi-Mid Tom";
            case 49: return "Crash Cymbal";    case 50: return "High Tom";
            case 51: return "Ride Cymbal";     case 52: return "Chinese Cymbal";
            case 53: return "Ride Bell";       case 54: return "Tambourine";
            case 55: return "Splash Cymbal";   case 56: return "Cowbell";
            case 57: return "Crash Cymbal 2";  case 59: return "Ride Cymbal 2";
            case 60: return "Hi Bongo";        case 61: return "Low Bongo";
            case 62: return "Mute Hi Conga";   case 63: return "Open Hi Conga";
            case 64: return "Low Conga";       case 65: return "High Timbale";
            case 66: return "Low Timbale";     case 67: return "High Agogo";
            case 69: return "Cabasa";          case 70: return "Maracas";
            case 75: return "Claves";          case 76: return "Hi Wood Block";
            default: return {};
        }
    }

    /** Label for a note: drum name if it maps to one, else pitch name. */
    inline juce::String label (int n, bool preferDrum)
    {
        if (preferDrum)
        {
            const auto d = gmDrum (n);
            if (d.isNotEmpty())
                return juce::String (n) + "  " + d;
        }
        return pitch (n);
    }
}
