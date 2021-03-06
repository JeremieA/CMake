/*============================================================================
  CMake - Cross Platform Makefile Generator
  Copyright 2012 Stephen Kelly <steveire@gmail.com>

  Distributed under the OSI-approved BSD License (the "License");
  see accompanying file Copyright.txt for details.

  This software is distributed WITHOUT ANY WARRANTY; without even the
  implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
  See the License for more information.
============================================================================*/
#ifndef cmGeneratorExpressionEvaluator_h
#define cmGeneratorExpressionEvaluator_h

#include <vector>
#include <string>

#include "cmListFileCache.h"

class cmTarget;

//----------------------------------------------------------------------------
struct cmGeneratorExpressionContext
{
  cmGeneratorExpressionContext()
    : Backtrace(NULL)
  {
  }

  cmListFileBacktrace Backtrace;
  std::set<cmTarget*> DependTargets;
  std::set<cmTarget const*> AllTargets;
  std::set<std::string> SeenTargetProperties;
  std::set<cmTarget const*> SourceSensitiveTargets;
  std::map<cmTarget const*, std::map<std::string, std::string> >
                                                          MaxLanguageStandard;
  cmMakefile *Makefile;
  std::string Config;
  std::string Language;
  cmTarget const* HeadTarget; // The target whose property is being evaluated.
  cmTarget const* CurrentTarget; // The dependent of HeadTarget which appears
                                 // directly or indirectly in the property.
  bool Quiet;
  bool HadError;
  bool HadContextSensitiveCondition;
  bool HadHeadSensitiveCondition;
  bool EvaluateForBuildsystem;
};

struct cmGeneratorExpressionDAGChecker;
struct cmGeneratorExpressionNode;

//----------------------------------------------------------------------------
struct cmGeneratorExpressionEvaluator
{
  cmGeneratorExpressionEvaluator() {}
  virtual ~cmGeneratorExpressionEvaluator() {}

  enum Type
  {
    Text,
    Generator
  };

  virtual Type GetType() const = 0;

  virtual std::string Evaluate(cmGeneratorExpressionContext *context,
                              cmGeneratorExpressionDAGChecker *) const = 0;

private:
  cmGeneratorExpressionEvaluator(const cmGeneratorExpressionEvaluator &);
  void operator=(const cmGeneratorExpressionEvaluator &);
};

struct TextContent : public cmGeneratorExpressionEvaluator
{
  TextContent(const char *start, size_t length)
    : Content(start), Length(length)
  {

  }

  std::string Evaluate(cmGeneratorExpressionContext *,
                       cmGeneratorExpressionDAGChecker *) const
  {
    return std::string(this->Content, this->Length);
  }

  Type GetType() const
  {
    return cmGeneratorExpressionEvaluator::Text;
  }

  void Extend(size_t length)
  {
    this->Length += length;
  }

  size_t GetLength()
  {
    return this->Length;
  }

private:
  const char *Content;
  size_t Length;
};

//----------------------------------------------------------------------------
struct GeneratorExpressionContent : public cmGeneratorExpressionEvaluator
{
  GeneratorExpressionContent(const char *startContent, size_t length);
  void SetIdentifier(std::vector<cmGeneratorExpressionEvaluator*> identifier)
  {
    this->IdentifierChildren = identifier;
  }

  void SetParameters(
        std::vector<std::vector<cmGeneratorExpressionEvaluator*> > parameters)
  {
    this->ParamChildren = parameters;
  }

  Type GetType() const
  {
    return cmGeneratorExpressionEvaluator::Generator;
  }

  std::string Evaluate(cmGeneratorExpressionContext *context,
                       cmGeneratorExpressionDAGChecker *) const;

  std::string GetOriginalExpression() const;

  ~GeneratorExpressionContent();

private:
  std::string EvaluateParameters(const cmGeneratorExpressionNode *node,
                                 const std::string &identifier,
                                 cmGeneratorExpressionContext *context,
                                 cmGeneratorExpressionDAGChecker *dagChecker,
                                 std::vector<std::string> &parameters) const;

  std::string ProcessArbitraryContent(
    const cmGeneratorExpressionNode *node,
    const std::string &identifier,
    cmGeneratorExpressionContext *context,
    cmGeneratorExpressionDAGChecker *dagChecker,
    std::vector<std::vector<cmGeneratorExpressionEvaluator*> >::const_iterator
    pit) const;

private:
  std::vector<cmGeneratorExpressionEvaluator*> IdentifierChildren;
  std::vector<std::vector<cmGeneratorExpressionEvaluator*> > ParamChildren;
  const char *StartContent;
  size_t ContentLength;
};

#endif
