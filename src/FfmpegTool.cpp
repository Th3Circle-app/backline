#include "FfmpegTool.h"

namespace FfmpegTool
{
    juce::File find()
    {
        const char* paths[] = { "/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg", "/usr/bin/ffmpeg" };
        for (auto* p : paths)
        {
            juce::File f (p);
            if (f.existsAsFile()) return f;
        }
        return {};
    }

    bool runSync (const juce::File& ffmpeg, const juce::StringArray& args, juce::String& output)
    {
        if (! ffmpeg.existsAsFile()) { output = "ffmpeg not found"; return false; }

        juce::StringArray full;
        full.add (ffmpeg.getFullPathName());
        full.addArray (args);

        juce::ChildProcess cp;
        if (! cp.start (full, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        {
            output = "failed to start ffmpeg";
            return false;
        }

        output = cp.readAllProcessOutput();
        cp.waitForProcessToFinish (-1);
        return cp.getExitCode() == 0;
    }

    std::vector<double> detectSceneCuts (const juce::File& ffmpeg, const juce::File& video, double threshold)
    {
        std::vector<double> cuts;
        juce::String out;
        const juce::String thr = juce::String (threshold);
        runSync (ffmpeg, { "-i", video.getFullPathName(),
                           "-vf", "select='gt(scene," + thr + ")',showinfo",
                           "-an", "-f", "null", "-" }, out);

        int idx = 0;
        while ((idx = out.indexOf (idx, "pts_time:")) >= 0)
        {
            idx += 9;
            juce::String num;
            int dots = 0;
            while (idx < out.length()
                   && (juce::CharacterFunctions::isDigit (out[idx]) || out[idx] == '.'))
            {
                if (out[idx] == '.') ++dots;
                num += out[idx];
                ++idx;
            }
            if (num.isNotEmpty() && dots <= 1)   // reject malformed tokens like "1.2.3"
            {
                const double v = num.getDoubleValue();
                if (v >= 0.0 && (cuts.empty() || v > cuts.back() + 1.0e-4))   // dedupe (showinfo is ordered)
                    cuts.push_back (v);
            }
        }
        return cuts;
    }
}
