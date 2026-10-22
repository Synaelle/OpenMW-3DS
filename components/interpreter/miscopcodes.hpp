#ifndef INTERPRETER_MISCOPCODES_H_INCLUDED
#define INTERPRETER_MISCOPCODES_H_INCLUDED

#include <algorithm>
#include <string_view>
#include <utility>
#include <format>
#include <string>
#include <vector>

#include "defines.hpp"
#include "opcodes.hpp"
#include "runtime.hpp"

#include <components/misc/messageformatparser.hpp>

namespace Interpreter
{
    class RuntimeMessageFormatter : public Misc::MessageFormatParser
    {
    private:
        std::string mFormattedMessage;
        Runtime& mRuntime;

    protected:
        void visitedPlaceholder(
            Placeholder placeholder, int flags, int width, int precision, Notation notation) override
        {
            std::string formatString;

            if (placeholder == StringPlaceholder)
            {
                int index = mRuntime[0].mInteger;
                mRuntime.pop();

                std::string_view value = mRuntime.getStringLiteral(index);
                if (precision >= 0)
                    value = value.substr(0, static_cast<std::size_t>(precision));
                if (width < 0)
                    mFormattedMessage += value;
                else
                {
                    formatString = "{:";
                    if (flags & PrependZero)
                        formatString += '0';
                    if (flags & AlignLeft)
                        formatString += '<';
                    else
                        formatString += '>';
                    formatString += "{}}";
                    std::vformat_to(
                        std::back_inserter(mFormattedMessage), formatString, std::make_format_args(value, width));
                }
            }
            else
            {
                formatString = "{:";
                if (flags & AlignLeft)
                    formatString += '<';
                if (flags & PositiveSign)
                    formatString += '+';
                else if (flags & PositiveSpace)
                    formatString += ' ';
                if (flags & AlternateForm)
                    formatString += '#';
                if (flags & PrependZero)
                    formatString += '0';
                if (width >= 0)
                    formatString += "{}";
                if (placeholder == FloatPlaceholder)
                {
                    if (precision >= 0)
                        formatString += ".{}";
                    formatString += static_cast<char>(notation);
                }
                else
                    precision = -1;
                formatString += '}';
                const auto appendMessage = [&](auto value) {
                    if (width >= 0 && precision >= 0)
                        std::vformat_to(std::back_inserter(mFormattedMessage), formatString,
                            std::make_format_args(value, width, precision));
                    else if (width >= 0)
                        std::vformat_to(
                            std::back_inserter(mFormattedMessage), formatString, std::make_format_args(value, width));
                    else if (precision >= 0)
                        std::vformat_to(std::back_inserter(mFormattedMessage), formatString,
                            std::make_format_args(value, precision));
                    else
                        std::vformat_to(
                            std::back_inserter(mFormattedMessage), formatString, std::make_format_args(value));
                };
                if (placeholder == FloatPlaceholder)
                {
                    float value = mRuntime[0].mFloat;
                    mRuntime.pop();
                    appendMessage(value);
                }
                else
                {
                    Type_Integer value = mRuntime[0].mInteger;
                    mRuntime.pop();
                    appendMessage(value);
                }
            }
        }

        void visitedCharacter(char c) override { mFormattedMessage += c; }

    public:
        RuntimeMessageFormatter(Runtime& runtime)
            : mRuntime(runtime)
        {
        }

        void process(std::string_view message) override
        {
            mFormattedMessage.clear();
            MessageFormatParser::process(message);
        }

        std::string getFormattedMessage() const { return mFormattedMessage; }
    };

    inline std::string formatMessage(std::string_view message, Runtime& runtime)
    {
        RuntimeMessageFormatter formatter(runtime);
        formatter.process(message);

        std::string formattedMessage = formatter.getFormattedMessage();
        formattedMessage = fixDefinesMsgBox(formattedMessage, runtime.getContext());
        return formattedMessage;
    }

#ifdef OPENMW_PLATFORM_3DS
    // The starting tutorial's messages (Morrowind.esm, the PC wording: "Right Clicking...", keys named after a
    // keyboard the 3DS doesn't have, "None and None move forward...") replaced with the 3DS's (OMW3DS l10n). Its
    // other wording (the Xbox function) names Xbox buttons.
    inline std::string_view tutorialMessage3DS(std::string_view message)
    {
        static constexpr std::pair<std::string_view, std::string_view> messages[] = {
            { "Right Clicking allows you to use your menus. When you are done with them, right click again to close "
              "them.",
                "#{OMW3DS:TutorialMenus}" },
            { "Read your papers by pressing the ^ActionActivate while looking at them. Then select 'Take' to pick "
              "them up.",
                "#{OMW3DS:TutorialPapers}" },
            { "Look up at the hatch and press ^ActionActivate to open it. ^ActionActivate activates objects and "
              "people you are looking at.",
                "#{OMW3DS:TutorialHatch}" },
            { "Press ^ActionActivate to talk to the Captain. Select topics to ask about them. Don't try persuasion "
              "on him. If you're not good at it, he will start to dislike you.",
                "#{OMW3DS:TutorialCaptain}" },
            { "Press ^ActionJournal to use your journal and review what you've been told. You should probably check "
              "out Arrille's Tradehouse up on the left. You're on your own now. Good luck.",
                "#{OMW3DS:TutorialJournal}" },
            { "You should learn how to do combat. Pick up the dagger on the table by activating it with "
              "^ActionActivate.",
                "#{OMW3DS:TutorialDagger}" },
            { "^ActionForward and ^ActionBack move forward and back. ^ActionSlideLeft and ^ActionSlideRight move "
              "side to side, and the mouse looks around.",
                "#{OMW3DS:TutorialMove}" },
        };
        for (const auto& [pc, port] : messages)
            if (message == pc)
                return port;
        return message;
    }
#endif

    class OpMessageBox : public Opcode1
    {
    public:
        void execute(Runtime& runtime, unsigned int arg0) override
        {
            // message
            int index = runtime[0].mInteger;
            runtime.pop();
            std::string_view message = runtime.getStringLiteral(index);
#ifdef OPENMW_PLATFORM_3DS
            message = tutorialMessage3DS(message);
#endif

            // buttons
            std::vector<std::string> buttons;

            for (std::size_t i = 0; i < arg0; ++i)
            {
                index = runtime[0].mInteger;
                runtime.pop();
                buttons.emplace_back(runtime.getStringLiteral(index));
            }

            std::reverse(buttons.begin(), buttons.end());

            // handle additional parameters
            std::string formattedMessage = formatMessage(message, runtime);

            runtime.getContext().messageBox(formattedMessage, buttons);
        }
    };

    class OpReport : public Opcode0
    {
    public:
        void execute(Runtime& runtime) override
        {
            // message
            int index = runtime[0].mInteger;
            runtime.pop();
            std::string_view message = runtime.getStringLiteral(index);

            // handle additional parameters
            std::string formattedMessage = formatMessage(message, runtime);

            runtime.getContext().report(formattedMessage);
        }
    };

}

#endif
