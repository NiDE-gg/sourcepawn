// vim: set sts=2 ts=8 sw=2 tw=99 et:
//
// Copyright (C) 2012-2014 David Anderson
//
// This file is part of SourcePawn.
//
// SourcePawn is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
// 
// SourcePawn is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with
// SourcePawn. If not, see http://www.gnu.org/licenses/.
#include "compile-context.h"
#include "compile-phases.h"
#include "source-manager.h"
#include "preprocessor.h"
#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace ke;

ThreadLocal<CompileContext *> ke::CurrentCompileContext;

CompileContext::CompileContext(int argc, char **argv)
  : outOfMemory_(false),
    strings_()
{
  assert(!CurrentCompileContext);

  CurrentCompileContext = this;

  source_ = new SourceManager(*this);

  if (argc < 2) {
    fprintf(stdout, "usage: <file>\n");
    return;
  }

  options_.InputFiles.append(argv[1]);

  // We automatically add "include" from the current working directory.
  options_.SearchPaths.append(AString("include/"));
}

CompileContext::~CompileContext()
{
  for (size_t i = 0; i < errors_.length(); i++)
    free(errors_[i].message);
  CurrentCompileContext = NULL;
}

bool
CompileContext::ChangePragmaDynamic(ReportingContext &rc, int64_t value)
{
  if (value < 0) {
    rc.reportError(Message_PragmaDynamicIsNegative);
    return false;
  }
  if (uint64_t(value) >= 64 * kMB) {
    rc.reportError(Message_PragmaDynamicIsTooLarge);
    return false;
  }

  options_.PragmaDynamic = size_t(value);
  return true;
}

static void
ReportMemory(FILE *fp)
{
  size_t allocated, reserved, bookkeeping;
  POOL().memoryUsage(&allocated, &reserved, &bookkeeping);

  fprintf(fp, " -- %" KE_FMT_SIZET " bytes allocated in pool\n", allocated);
  fprintf(fp, " -- %" KE_FMT_SIZET " bytes reserved in pool\n", reserved);
  fprintf(fp, " -- %" KE_FMT_SIZET " bytes used for bookkeeping\n", bookkeeping);
}

bool
CompileContext::compile()
{
  if (!strings_.init())
    return false;
  if (!types_.initialize())
    return false;

  ReportingContext rc(*this, SourceLocation());
  Ref<SourceFile> file = source_->open(rc, options_.InputFiles[0].chars());
  if (!file)
    return false;

  Preprocessor pp(*this, options_);

  fprintf(stderr, "-- Parsing --\n");

  TranslationUnit *unit = new (pool()) TranslationUnit();
  {
    if (!pp.enter(file))
      return false;

    Parser p(*this, pp, options_);
    ParseTree *tree = p.parse();
    if (!tree || errors_.length())
      return false;

    pp.leave();
    if (errors_.length())
      return false;

    unit->attach(tree);
    //tree->dump(stdout);
  }

  ReportMemory(stderr);

  fprintf(stderr, "\n-- Name Binding --\n");

  if (!ResolveNames(*this, unit))
    return false;

  ReportMemory(stderr);

  fprintf(stderr, "\n-- Type Resolution --\n");

  if (!ResolveTypes(*this, unit))
    return false;

  ReportMemory(stderr);

  //units_[0]->tree()->dump(stdout);

  {
    //AmxEmitter sema(*this, units_[0]);
    //if (!sema.compile())
    //  return false;
  }

  if (errors_.length())
    return false;
  if (outOfMemory_)
    return false;

  return true;
}

const MessageInfo ke::Messages[] =
{
#define MSG(Name, Type, String) \
    { MessageType_##Type, String },
# include "messages.tbl"
#undef MSG
    { MessageType_SyntaxError, NULL }
};

const char *ke::MessageTypes[] =
{
  "syntax",
  "type",
  "system"
};

#if defined(_MSC_VER)
# define VA_COPY(to, from)  to = from
#else
# define VA_COPY(to, from)  va_copy(to, from)
#endif

static char *
BuildErrorMessage(const char *format, va_list ap)
{
  va_list use;
  VA_COPY(use, ap);

  // We over-allocate and zero terminate ahead of time, since LIBCRT's
  // version of vsnprintf is screwy.
  size_t size = 255;
  char *buffer = (char *)calloc(size + 1, 1);
  if (!buffer)
    return NULL;

  for (;;) {
    int r = vsnprintf(buffer, size, format, use);

    if (r > -1 && size_t(r) < size)
      return buffer;
     
#if defined(_MSC_VER)
    if (r < 0)
      size *= 2;
#else
    if (r > -1)
      size = size_t(r) + 1;
    else
      size *= 2;
#endif

    free(buffer);
    if ((buffer = (char *)calloc(size + 1, 1)) == NULL)
      return NULL;

    VA_COPY(use, ap);
  }
}

void
CompileContext::reportErrorVa(const SourceLocation &loc, Message msg, va_list ap)
{
  char *message = BuildErrorMessage(Messages[msg].format, ap);

  CompileError report;
  report.loc = loc;
  report.type = MessageTypes[Messages[msg].type];
  report.message = message;
  errors_.append(report);
}

static char *
FormatString(const char *format, ...)
{
  va_list ap;
  va_start(ap, format);
  char *str = BuildErrorMessage(format, ap);
  va_end(ap);
  return str;
}

Atom *
CompileContext::createAnonymousName(const SourceLocation &loc)
{
  // :SRCLOC: include file name
  AutoArray<char> message(
      FormatString("anonymous at %d:%d",
                   source_->getLine(loc),
                   source_->getCol(loc)));
  return add(message);
}

void
CompileContext::reportError(const SourceLocation &loc, Message msg, ...)
{
  va_list ap;
  va_start(ap, msg);
  reportErrorVa(loc, msg, ap);
  va_end(ap);
}

void
ReportingContext::reportError(Message msg, ...)
{
  if (!should_error_)
    return;

  va_list ap;
  va_start(ap, msg);
  cc_.reportErrorVa(loc_, msg, ap);
  va_end(ap);
}

#if defined __linux__
extern "C" void __cxa_pure_virtual(void)
{
}

void *operator new(size_t size)
{
	return malloc(size);
}

void *operator new[](size_t size) 
{
	return malloc(size);
}

void operator delete(void *ptr) 
{
	free(ptr);
}

void operator delete[](void * ptr)
{
	free(ptr);
}
#endif

