/*
 * Cppcheck - A tool for static C/C++ code analysis
 * Copyright (C) 2007-2019 Cppcheck team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "errorlogger.h"

#include "cppcheck.h"
#include "mathlib.h"
#include "path.h"
#include "token.h"
#include "tokenlist.h"
#include "utils.h"
#include "settings.h"

#include <tinyxml2.h>
#include <array>
#include <cassert>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iomanip>

InternalError::InternalError(const Token *tok, const std::string &errorMsg, Type type) :
    token(tok), errorMessage(errorMsg), type(type)
{
    switch (type) {
    case AST:
        id = "internalAstError";
        break;
    case SYNTAX:
        id = "syntaxError";
        break;
    case UNKNOWN_MACRO:
        id = "unknownMacro";
        break;
    case INTERNAL:
        id = "ctrlppcheckError";
        break;
    }
}

ErrorLogger::ErrorMessage::ErrorMessage()
    : _severity(Severity::none), _cwe(0U), _inconclusive(false)
{
}

ErrorLogger::ErrorMessage::ErrorMessage(const std::list<FileLocation> &callStack, const std::string& file1, Severity::SeverityType severity, const std::string &msg, const std::string &id, bool inconclusive) :
    _callStack(callStack), // locations for this error message
    _id(id),               // set the message id
    file0(file1),
    _severity(severity),   // severity for this error message
    _cwe(0U),
    _inconclusive(inconclusive)
{
    // set the summary and verbose messages
    setmsg(msg);
}



ErrorLogger::ErrorMessage::ErrorMessage(const std::list<FileLocation> &callStack, const std::string& file1, Severity::SeverityType severity, const std::string &msg, const std::string &id, const CWE &cwe, bool inconclusive) :
    _callStack(callStack), // locations for this error message
    _id(id),               // set the message id
    file0(file1),
    _severity(severity),   // severity for this error message
    _cwe(cwe.id),
    _inconclusive(inconclusive)
{
    // set the summary and verbose messages
    setmsg(msg);
}

ErrorLogger::ErrorMessage::ErrorMessage(const std::list<const Token*>& callstack, const TokenList* list, Severity::SeverityType severity, const std::string& id, const std::string& msg, bool inconclusive)
    : _id(id), _severity(severity), _cwe(0U), _inconclusive(inconclusive)
{
    // Format callstack
    for (std::list<const Token *>::const_iterator it = callstack.begin(); it != callstack.end(); ++it) {
        // --errorlist can provide null values here
        if (!(*it))
            continue;

        _callStack.emplace_back(*it, list);
    }

    if (list && !list->getFiles().empty())
        file0 = list->getFiles()[0];

    setmsg(msg);
}


ErrorLogger::ErrorMessage::ErrorMessage(const std::list<const Token*>& callstack, const TokenList* list, Severity::SeverityType severity, const std::string& id, const std::string& msg, const CWE &cwe, bool inconclusive)
    : _id(id), _severity(severity), _cwe(cwe.id), _inconclusive(inconclusive)
{
    // Format callstack
    for (std::list<const Token *>::const_iterator it = callstack.begin(); it != callstack.end(); ++it) {
        // --errorlist can provide null values here
        if (!(*it))
            continue;

        _callStack.emplace_back(*it, list);
    }

    if (list && !list->getFiles().empty())
        file0 = list->getFiles()[0];

    setmsg(msg);
}

ErrorLogger::ErrorMessage::ErrorMessage(const ErrorPath &errorPath, const TokenList *tokenList, Severity::SeverityType severity, const char id[], const std::string &msg, const CWE &cwe, bool inconclusive)
    : _id(id), _severity(severity), _cwe(cwe.id), _inconclusive(inconclusive)
{
    // Format callstack
    for (ErrorPath::const_iterator it = errorPath.begin(); it != errorPath.end(); ++it) {
        const Token *tok = it->first;
        const std::string &info = it->second;

        // --errorlist can provide null values here
        if (tok)
            _callStack.emplace_back(tok, info, tokenList);
    }

    if (tokenList && !tokenList->getFiles().empty())
        file0 = tokenList->getFiles()[0];

    setmsg(msg);
}

ErrorLogger::ErrorMessage::ErrorMessage(const tinyxml2::XMLElement * const errmsg)
    : _severity(Severity::none),
      _cwe(0U),
      _inconclusive(false)
{
    const char * const unknown = "<UNKNOWN>";

    const char *attr = errmsg->Attribute("id");
    _id = attr ? attr : unknown;

    attr = errmsg->Attribute("severity");
    _severity = attr ? Severity::fromString(attr) : Severity::none;

    attr = errmsg->Attribute("cwe");
    std::istringstream(attr ? attr : "0") >> _cwe.id;

    attr = errmsg->Attribute("inconclusive");
    _inconclusive = attr && (std::strcmp(attr, "true") == 0);

    attr = errmsg->Attribute("msg");
    mShortMessage = attr ? attr : "";

    attr = errmsg->Attribute("verbose");
    mVerboseMessage = attr ? attr : "";

    for (const tinyxml2::XMLElement *e = errmsg->FirstChildElement(); e; e = e->NextSiblingElement()) {
        if (std::strcmp(e->Name(),"location")==0) {
            const char *strfile = e->Attribute("file");
            const char *strinfo = e->Attribute("info");
            const char *strline = e->Attribute("line");

            const char *file = strfile ? strfile : unknown;
            const char *info = strinfo ? strinfo : "";
            const int line = strline ? std::atoi(strline) : 0;
            _callStack.emplace_back(file, info, line);
        }
    }
}

void ErrorLogger::ErrorMessage::setmsg(const std::string &msg)
{
    // If a message ends to a '\n' and contains only a one '\n'
    // it will cause the mVerboseMessage to be empty which will show
    // as an empty message to the user if --verbose is used.
    // Even this doesn't cause problems with messages that have multiple
    // lines, none of the error messages should end into it.
    assert(!endsWith(msg,'\n'));

    // The summary and verbose message are separated by a newline
    // If there is no newline then both the summary and verbose messages
    // are the given message
    const std::string::size_type pos = msg.find('\n');
    const std::string symbolName = mSymbolNames.empty() ? std::string() : mSymbolNames.substr(0, mSymbolNames.find('\n'));
    if (pos == std::string::npos) {
        mShortMessage = replaceStr(msg, "$symbol", symbolName);
        mVerboseMessage = replaceStr(msg, "$symbol", symbolName);
    } else if (msg.compare(0,8,"$symbol:") == 0) {
        mSymbolNames += msg.substr(8, pos-7);
        setmsg(msg.substr(pos + 1));
    } else {
        mShortMessage = replaceStr(msg.substr(0, pos), "$symbol", symbolName);
        mVerboseMessage = replaceStr(msg.substr(pos + 1), "$symbol", symbolName);
    }
}

Suppressions::ErrorMessage ErrorLogger::ErrorMessage::toSuppressionsErrorMessage() const
{
    Suppressions::ErrorMessage ret;
    ret.errorId = _id;
    if (!_callStack.empty()) {
        ret.setFileName(_callStack.back().getfile(false));
        ret.lineNumber = _callStack.back().line;
    }
    ret.inconclusive = _inconclusive;
    ret.symbolNames = mSymbolNames;
    return ret;
}

std::string ErrorLogger::ErrorMessage::getXMLHeader()
{
    // xml_version 1 is the default xml format

    tinyxml2::XMLPrinter printer;

    // standard xml header
    printer.PushDeclaration("xml version=\"1.0\" encoding=\"UTF-8\"");

    // header
    printer.OpenElement("results", false);

    printer.PushAttribute("version", 2);
    printer.OpenElement("cppcheck", false);
    printer.PushAttribute("version", CppCheck::version());
    printer.CloseElement(false);
    printer.OpenElement("errors", false);

    return std::string(printer.CStr()) + '>';
}

std::string ErrorLogger::ErrorMessage::getXMLFooter()
{
    return "    </errors>\n</results>";
}

// There is no utf-8 support around but the strings should at least be safe for to tinyxml2.
// See #5300 "Invalid encoding in XML output" and  #6431 "Invalid XML created - Invalid encoding of string literal "
std::string ErrorLogger::ErrorMessage::fixInvalidChars(const std::string& raw)
{
    std::string result;
    result.reserve(raw.length());
    std::string::const_iterator from=raw.begin();
    while (from!=raw.end()) {
        if (std::isprint(static_cast<unsigned char>(*from))) {
            result.push_back(*from);
        } else {
            std::ostringstream es;
            // straight cast to (unsigned) doesn't work out.
            const unsigned uFrom = (unsigned char)*from;
            es << '\\' << std::setbase(8) << std::setw(3) << std::setfill('0') << uFrom;
            result += es.str();
        }
        ++from;
    }
    return result;
}

std::string ErrorLogger::ErrorMessage::toXML() const
{
    tinyxml2::XMLPrinter printer(nullptr, false, 2);
    printer.OpenElement("error", false);
    printer.PushAttribute("id", _id.c_str());
    printer.PushAttribute("severity", Severity::toString(_severity).c_str());
    printer.PushAttribute("msg", fixInvalidChars(mShortMessage).c_str());
    printer.PushAttribute("verbose", fixInvalidChars(mVerboseMessage).c_str());
    if (_cwe.id)
        printer.PushAttribute("cwe", _cwe.id);
    if (_inconclusive)
        printer.PushAttribute("inconclusive", "true");

    for (std::list<FileLocation>::const_reverse_iterator it = _callStack.rbegin(); it != _callStack.rend(); ++it) {
        printer.OpenElement("location", false);
        if (!file0.empty() && (*it).getfile() != file0)
            printer.PushAttribute("file0", Path::toNativeSeparators(file0).c_str());
        printer.PushAttribute("file", (*it).getfile().c_str());
        printer.PushAttribute("line", std::max((*it).line,0));
        if (!it->getinfo().empty())
            printer.PushAttribute("info", fixInvalidChars(it->getinfo()).c_str());
        printer.CloseElement(false);
    }
    for (std::string::size_type pos = 0; pos < mSymbolNames.size();) {
        const std::string::size_type pos2 = mSymbolNames.find('\n', pos);
        std::string symbolName;
        if (pos2 == std::string::npos) {
            symbolName = mSymbolNames.substr(pos);
            pos = pos2;
        } else {
            symbolName = mSymbolNames.substr(pos, pos2-pos);
            pos = pos2 + 1;
        }
        printer.OpenElement("symbol", false);
        printer.PushText(symbolName.c_str());
        printer.CloseElement(false);
    }
    printer.CloseElement(false);
    return printer.CStr();
}

void ErrorLogger::ErrorMessage::findAndReplace(std::string &source, const std::string &searchFor, const std::string &replaceWith)
{
    std::string::size_type index = 0;
    while ((index = source.find(searchFor, index)) != std::string::npos) {
        source.replace(index, searchFor.length(), replaceWith);
        index += replaceWith.length();
    }
}

// TODO: read info from some shared resource instead?
static std::string readCode(const std::string &file, unsigned int linenr, unsigned int column, const char endl[])
{
    std::ifstream fin(file);
    std::string line;
    while (linenr > 0 && std::getline(fin,line)) {
        linenr--;
    }
    const std::string::size_type endPos = line.find_last_not_of("\r\n\t ");
    if (endPos + 1 < line.size())
        line.erase(endPos + 1);
    std::string::size_type pos = 0;
    while ((pos = line.find('\t', pos)) != std::string::npos)
        line[pos] = ' ';
    return line + endl + std::string((column>0 ? column-1 : column), ' ') + '^';
}

std::string ErrorLogger::ErrorMessage::toString(bool verbose, const std::string &templateFormat, const std::string &templateLocation) const
{
    // Save this ErrorMessage in plain text.

    // No template is given
    if (templateFormat.empty()) {
        std::ostringstream text;
        if (!_callStack.empty())
            text << callStackToString(_callStack) << ": ";
        if (_severity != Severity::none) {
            text << '(' << Severity::toString(_severity);
            if (_inconclusive)
                text << ", inconclusive";
            text << ") ";
        }
        text << (verbose ? mVerboseMessage : mShortMessage);
        return text.str();
    }

    // template is given. Reformat the output according to it
    std::string result = templateFormat;
    // Support a few special characters to allow to specific formatting, see http://sourceforge.net/apps/phpbb/cppcheck/viewtopic.php?f=4&t=494&sid=21715d362c0dbafd3791da4d9522f814
    // Substitution should be done first so messages from cppcheck never get translated.
    findAndReplace(result, "\\b", "\b");
    findAndReplace(result, "\\n", "\n");
    findAndReplace(result, "\\r", "\r");
    findAndReplace(result, "\\t", "\t");

    findAndReplace(result, "{id}", _id);
    if (result.find("{inconclusive:") != std::string::npos) {
        const std::string::size_type pos1 = result.find("{inconclusive:");
        const std::string::size_type pos2 = result.find('}', pos1+1);
        const std::string replaceFrom = result.substr(pos1,pos2-pos1+1);
        const std::string replaceWith = _inconclusive ? result.substr(pos1+14, pos2-pos1-14) : std::string();
        findAndReplace(result, replaceFrom, replaceWith);
    }
    findAndReplace(result, "{severity}", Severity::toString(_severity));
    findAndReplace(result, "{cwe}", MathLib::toString(_cwe.id));
    findAndReplace(result, "{message}", verbose ? mVerboseMessage : mShortMessage);
    findAndReplace(result, "{callstack}", _callStack.empty() ? emptyString : callStackToString(_callStack));
    if (!_callStack.empty()) {
        findAndReplace(result, "{file}", _callStack.back().getfile());
        findAndReplace(result, "{line}", MathLib::toString(_callStack.back().line));
        findAndReplace(result, "{column}", MathLib::toString(_callStack.back().col));
        if (result.find("{code}") != std::string::npos) {
            const std::string::size_type pos = result.find('\r');
            const char *endl;
            if (pos == std::string::npos)
                endl = "\n";
            else if (pos+1 < result.size() && result[pos+1] == '\n')
                endl = "\r\n";
            else
                endl = "\r";
            findAndReplace(result, "{code}", readCode(_callStack.back().getOrigFile(), _callStack.back().line, _callStack.back().col, endl));
        }
    } else {
        findAndReplace(result, "{file}", "nofile");
        findAndReplace(result, "{line}", "0");
        findAndReplace(result, "{column}", "0");
        findAndReplace(result, "{code}", emptyString);
    }

    if (!templateLocation.empty() && _callStack.size() >= 2U) {
        for (const FileLocation &fileLocation : _callStack) {
            std::string text = templateLocation;

            findAndReplace(text, "\\b", "\b");
            findAndReplace(text, "\\n", "\n");
            findAndReplace(text, "\\r", "\r");
            findAndReplace(text, "\\t", "\t");

            findAndReplace(text, "{file}", fileLocation.getfile());
            findAndReplace(text, "{line}", MathLib::toString(fileLocation.line));
            findAndReplace(text, "{column}", MathLib::toString(fileLocation.col));
            findAndReplace(text, "{info}", fileLocation.getinfo().empty() ? mShortMessage : fileLocation.getinfo());
            if (text.find("{code}") != std::string::npos) {
                const std::string::size_type pos = text.find('\r');
                const char *endl;
                if (pos == std::string::npos)
                    endl = "\n";
                else if (pos+1 < text.size() && text[pos+1] == '\n')
                    endl = "\r\n";
                else
                    endl = "\r";
                findAndReplace(text, "{code}", readCode(fileLocation.getOrigFile(), fileLocation.line, fileLocation.col, endl));
            }
            result += '\n' + text;
        }
    }

    return result;
}

bool ErrorLogger::reportUnmatchedSuppressions(const std::list<Suppressions::Suppression> &unmatched)
{
    bool err = false;
    // Report unmatched suppressions
    for (const Suppressions::Suppression &s : unmatched) {
        // don't report "unmatchedSuppression" as unmatched
        if (s.errorId == "unmatchedSuppression")
            continue;

        // check if this unmatched suppression is suppressed
        bool suppressed = false;
        for (const Suppressions::Suppression &s2 : unmatched) {
            if (s2.errorId == "unmatchedSuppression") {
                if ((s2.fileName == "*" || s2.fileName == s.fileName) &&
                    (s2.lineNumber == Suppressions::Suppression::NO_LINE || s2.lineNumber == s.lineNumber)) {
                    suppressed = true;
                    break;
                }
            }
        }

        if (suppressed)
            continue;

        std::list<ErrorLogger::ErrorMessage::FileLocation> callStack;
        if (!s.fileName.empty())
            callStack.emplace_back(s.fileName, s.lineNumber);
        reportErr(ErrorLogger::ErrorMessage(callStack, emptyString, Severity::information, "Unmatched suppression: " + s.errorId, "unmatchedSuppression", false));
        err = true;
    }
    return err;
}

std::string ErrorLogger::callStackToString(const std::list<ErrorLogger::ErrorMessage::FileLocation> &callStack)
{
    std::ostringstream ostr;
    for (std::list<ErrorLogger::ErrorMessage::FileLocation>::const_iterator tok = callStack.begin(); tok != callStack.end(); ++tok) {
        ostr << (tok == callStack.begin() ? "" : " -> ") << tok->stringify();
    }
    return ostr.str();
}


ErrorLogger::ErrorMessage::FileLocation::FileLocation(const Token* tok, const TokenList* tokenList)
    : fileIndex(tok->fileIndex()), line(tok->linenr()), col(tok->col()), mOrigFileName(tokenList->getOrigFile(tok)), mFileName(tokenList->file(tok))
{
}

ErrorLogger::ErrorMessage::FileLocation::FileLocation(const Token* tok, const std::string &info, const TokenList* tokenList)
    : fileIndex(tok->fileIndex()), line(tok->linenr()), col(tok->col()), mOrigFileName(tokenList->getOrigFile(tok)), mFileName(tokenList->file(tok)), mInfo(info)
{
}

std::string ErrorLogger::ErrorMessage::FileLocation::getfile(bool convert) const
{
    if (convert)
        return Path::toNativeSeparators(mFileName);
    return mFileName;
}

std::string ErrorLogger::ErrorMessage::FileLocation::getOrigFile(bool convert) const
{
    if (convert)
        return Path::toNativeSeparators(mOrigFileName);
    return mOrigFileName;
}

void ErrorLogger::ErrorMessage::FileLocation::setfile(const std::string &file)
{
    mFileName = file;
    mFileName = Path::fromNativeSeparators(mFileName);
    mFileName = Path::simplifyPath(mFileName);
}

std::string ErrorLogger::ErrorMessage::FileLocation::stringify() const
{
    std::ostringstream oss;
    oss << '[' << Path::toNativeSeparators(mFileName);
    if (line != Suppressions::Suppression::NO_LINE)
        oss << ':' << line;
    oss << ']';
    return oss.str();
}

std::string ErrorLogger::toxml(const std::string &str)
{
    std::ostringstream xml;
    for (std::size_t i = 0U; i < str.length(); i++) {
        const unsigned char c = str[i];
        switch (c) {
        case '<':
            xml << "&lt;";
            break;
        case '>':
            xml << "&gt;";
            break;
        case '&':
            xml << "&amp;";
            break;
        case '\"':
            xml << "&quot;";
            break;
        case '\0':
            xml << "\\0";
            break;
        case '\n':
            xml << "\\n";
            break;

        default:
            if (c >= ' ' && c <= 0x7f)
                xml << c;
            else
                xml << 'x';
            break;
        }
    }
    return xml.str();
}

std::string replaceStr(std::string s, const std::string &from, const std::string &to)
{
    std::string::size_type pos1 = 0;
    while (pos1 < s.size()) {
        pos1 = s.find(from, pos1);
        if (pos1 == std::string::npos)
            return s;
        if (pos1 > 0 && (s[pos1-1] == '_' || std::isalnum(s[pos1-1]))) {
            pos1++;
            continue;
        }
        const std::string::size_type pos2 = pos1 + from.size();
        if (pos2 >= s.size())
            return s.substr(0,pos1) + to;
        if (s[pos2] == '_' || std::isalnum(s[pos2])) {
            pos1++;
            continue;
        }
        s = s.substr(0,pos1) + to + s.substr(pos2);
        pos1 += to.size();
    }
    return s;
}
