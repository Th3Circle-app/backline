#pragma once

#include <juce_core/juce_core.h>

//==============================================================================
/** Built-in stem separation (a.k.a. "stem splitter"): splits a finished song
    into its parts (vocals / drums / bass / guitar / piano / other) so we can
    work with material that arrived without stems.

    Backed by Demucs (Meta, MIT-licensed, state of the art) running in a private
    Python venv that the app sets up once at:
        ~/Library/Application Support/Layback/demucs-venv

    We shell out to it (same pattern as FfmpegTool) so the heavy ML stays out of
    the C++ process. This is the "backend" boundary: a future native demucs.cpp
    build can replace separate() without touching the UI or import flow. */
namespace StemSplitter
{
    /** Root of our managed Demucs venv (per-OS app-data dir: ~/Library/Application
        Support/Layback on macOS, %APPDATA%/Layback on Windows, ~/.config on Linux). */
    inline juce::File venvDir()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("Layback").getChildFile ("demucs-venv");
    }

    /** The python interpreter inside our managed Demucs venv (cross-platform layout). */
    inline juce::File venvPython()
    {
       #if JUCE_WINDOWS
        return venvDir().getChildFile ("Scripts").getChildFile ("python.exe");
       #else
        return venvDir().getChildFile ("bin").getChildFile ("python");
       #endif
    }

    /** True once the venv exists (the one-time Demucs install has completed). */
    inline bool isInstalled() { return venvPython().existsAsFile(); }

    /** Locate the `uv` installer binary used to build the venv (it fetches its own
        Python 3.11 + installs Demucs). Returns an invalid File if not found. */
    inline juce::File findUv()
    {
        const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
        juce::Array<juce::File> candidates;
       #if JUCE_WINDOWS
        candidates.add (home.getChildFile (".local/bin/uv.exe"));
        candidates.add (home.getChildFile ("AppData/Local/Microsoft/WinGet/Links/uv.exe"));
        candidates.add (juce::File ("C:/Program Files/uv/uv.exe"));
       #else
        candidates.add (home.getChildFile (".local/bin/uv"));
        candidates.add (juce::File ("/usr/local/bin/uv"));
        candidates.add (juce::File ("/opt/homebrew/bin/uv"));
        candidates.add (juce::File ("/usr/bin/uv"));
       #endif
        for (auto& f : candidates) if (f.existsAsFile()) return f;
        return {};
    }

    /** The stems a given model produces, in Demucs' output order. */
    inline juce::StringArray stemNames (bool sixStem)
    {
        return sixStem ? juce::StringArray { "vocals", "drums", "bass", "guitar", "piano", "other" }
                       : juce::StringArray { "vocals", "drums", "bass", "other" };
    }

    inline juce::String modelName (bool sixStem) { return sixStem ? "htdemucs_6s" : "htdemucs"; }

    /** Runs Demucs SYNCHRONOUSLY (call from a worker thread). On success, fills
        outStems with the produced WAV files and returns true; otherwise sets
        error. The first run for a model also downloads its weights (needs net). */
    inline bool separate (const juce::File& input, const juce::File& outDir,
                          bool sixStem, juce::String& error, juce::Array<juce::File>& outStems)
    {
        outStems.clear();
        const auto py = venvPython();
        if (! py.existsAsFile()) { error = "The stem splitter isn't installed yet (Demucs venv missing)."; return false; }
        if (! input.existsAsFile()) { error = "Source audio file not found."; return false; }
        outDir.createDirectory();

        const juce::String model = modelName (sixStem);
        juce::StringArray cmd;
        cmd.add (py.getFullPathName());
        cmd.add ("-m");    cmd.add ("demucs");
        cmd.add ("-n");    cmd.add (model);
        cmd.add ("-d");    cmd.add ("cpu");              // x86 mac: CPU inference (offline render)
        cmd.add ("--out"); cmd.add (outDir.getFullPathName());
        cmd.add (input.getFullPathName());

        juce::ChildProcess proc;
        if (! proc.start (cmd, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        { error = "Could not launch Demucs."; return false; }

        const juce::String log = proc.readAllProcessOutput();   // blocks until Demucs finishes

        // Demucs writes <outDir>/<model>/<inputBaseName>/<stem>.wav
        const auto stemDir = outDir.getChildFile (model).getChildFile (input.getFileNameWithoutExtension());
        for (auto& nm : stemNames (sixStem))
        {
            const auto f = stemDir.getChildFile (nm + ".wav");
            if (f.existsAsFile()) outStems.add (f);
        }
        if (outStems.isEmpty())
        {
            error = "Demucs produced no stems. " + log.getLastCharacters (300);
            return false;
        }
        return true;
    }
}
