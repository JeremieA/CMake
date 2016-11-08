
/*============================================================================
	CMake - Cross Platform Makefile Generator
	Copyright 2000-2009 Kitware, Inc., Insight Software Consortium

	Distributed under the OSI-approved BSD License (the "License");
	see accompanying file Copyright.txt for details.

	This software is distributed WITHOUT ANY WARRANTY; without even the
	implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
	See the License for more information.
============================================================================*/
/*============================================================================
	Development progress:

	Tasks/Issues:
	 - Execute unit tests against the generator somehow
	 - Fix target aliases being repeated in the output
	 - Fix cmake build using fastbuild (currently appears configuration incorrect)
	   -> seems to be working now :)
	 - Running some of the Cmake generation, the pdb files can't be deleted (shows up errors)
	 - Depends upon visual studio generator code to sort dependencies
	 - When generating CMAKE from scratch, it sometimes errors with fortran complaints and fails generation?
	 a re-run will succeed.
	 - Linker for msvc uses the cmake command to callback. Don't think this is an issue since
	 I think compilation is the part that gets distributed anyway.
	 But it might mean that the cache has trouble calculating deps for obj->lib/exe.
	 Not sure if Fastbuild supports that anyway yet
	 - Need to sort custom build commands by their outputs
	 - Unit tests BuildDepends, RunCMake.Configure, and requires auto-rerun of cmake when detecting changes to
	   source scripts. Not implementable unless FASTBuild can detect when a rule outputs the current bff file and
	   reload it afterwards ?
	 - Unit test SimpleInstall (and SimpleInstall-Stage2) call cmake --build . as post-build commands,
	   leading to FASTBuild to exit with error "FBuild: Error: Another instance of FASTBuild is already running in".

	Fastbuild bugs:
	 - Defining prebuild dependencies that don't exist, causes the error output when that
	 target is actually defined. Rather than originally complaining that the target
	 doesn't exist where the reference is attempted.
	 - Parsing strings with double $$ doesn't generate a nice error
	 - Undocumented that you can escape a $ with ^$
	   -> now documented in http://www.fastbuild.org/docs/syntaxguide.html#escaping
	 - ExecInputs is invalid empty
	 - Would be great if you could define dummy targets (maybe blank aliases?)
	 - Exec nodes need to not worry about dummy output files not being created
	 - Would be nice if nodes didn't need to be completely in order. But then cycles would be possible
	 - Implib directory is not created for exeNodes (DLLs work now though)
	 - Issues with preserving escaped quotes and double spaces, such as in -DMYVAR="\"string  with 2 spaces\""
	   -> disabled it from Preprocess unit test, but it would be cleaner to find a way to fix it instead

	Limitations:
	 - Only tested/working with MSVC

	Notes:
	 - Understanding Custom Build Steps and Build Events
		 https://msdn.microsoft.com/en-us/library/e85wte0k.aspx
	 very useful documentation detailing the order of execution
	 of standard MSVC events. This is useful to determine correct
	 behaviour of fastbuild generator (view a project generated to MSVC,
	 then apply the same rules/assumptions back into fastbuild).
	 i.e. Custom rules are always executed first.

	Current list of unit tests failing:

	91% tests passed, 32 tests failed out of 371
	Total Test time (real) = 1479.86 sec

The following tests FAILED:
	 59 - Preprocess (Failed)
	 60 - ExportImport (Failed)
	 68 - StagingPrefix (Failed)
	 70 - ConfigSources (Failed)
	 79 - Module.GenerateExportHeader (Failed)
	101 - SubProject-Stage2 (Failed)
	113 - BuildDepends (Failed)
	114 - SimpleInstall (Failed)
	115 - SimpleInstall-Stage2 (Failed)
	127 - complex (Failed)
	128 - complexOneConfig (Failed)
	131 - ExternalProject (Failed)
	132 - ExternalProjectLocal (Failed)
	133 - ExternalProjectUpdateSetup (Failed)
	134 - ExternalProjectUpdate (Failed)
	151 - Plugin (Failed)
	157 - PrecompiledHeader (Failed)
	184 - CTestConfig.Script.Debug (Failed)
	185 - CTestConfig.Dashboard.Debug (Failed)
	186 - CTestConfig.Script.MinSizeRel (Failed)
	187 - CTestConfig.Dashboard.MinSizeRel (Failed)
	188 - CTestConfig.Script.Release (Failed)
	189 - CTestConfig.Dashboard.Release (Failed)
	190 - CTestConfig.Script.RelWithDebInfo (Failed)
	191 - CTestConfig.Dashboard.RelWithDebInfo (Failed)
	197 - CMakeCommands.target_compile_options (Failed)
	238 - CMakeOnly.CheckStructHasMember (Failed)
	275 - RunCMake.Configure (Failed)
	331 - RunCMake.File_Generate (Failed)
	371 - CMake.CheckSourceTree (Failed)
============================================================================*/
#include "cmGlobalFastbuildGenerator.h"

#include "cmGeneratorTarget.h"
#include "cmGlobalGeneratorFactory.h"
#include "cmLocalFastbuildGenerator.h"
#include "cmMakefile.h"
#include "cmSourceFile.h"
#include "cmTarget.h"
#include "cmGeneratedFileStream.h"
#include "cmLocalGenerator.h"
#include "cmComputeLinkInformation.h"
#include "cmGlobalVisualStudioGenerator.h"
#include "cmCustomCommandGenerator.h"
#include <cmsys/Encoding.hxx>
#include <assert.h>

static const char fastbuildGeneratorName[] = "Fastbuild";

static std::string VSDebugConfiguration = "VDebug";
static std::string VSRelWithDebInfoConfiguration = "_RelWithDebInfo";

// Simple trace to help debug unit tests
#define FBTRACE(s)
//#define FBTRACE(s) cmSystemTools::Stdout(s)

void cmGlobalFastbuildGenerator::GetDocumentation(cmDocumentationEntry& entry)
{
	entry.Name = fastbuildGeneratorName;
	entry.Brief = "Generates fastbuild project files.";
}

std::string cmGlobalFastbuildGenerator::GetActualName()
{
	return fastbuildGeneratorName;
}

//----------------------------------------------------------------------------
class cmGlobalFastbuildGenerator::Detail
{
public:
	class FileContext;
	class Definition;
	class Detection;
	class Generation;
};

//----------------------------------------------------------------------------
class cmGlobalFastbuildGenerator::Detail::FileContext
{
public:
	FileContext(cmGeneratedFileStream & afout)
		: fout(afout)
	{
	}

	void WriteComment(const std::string& comment)
	{
		fout << linePrefix << ";" << comment << "\n";
	}

	void WriteCommentMultiLines(std::string comment)
	{
		if (!comment.empty() && comment.back() == '\n')
			comment.pop_back(); // remove last EOL
		cmSystemTools::ReplaceString(comment, "\n", "\n" + linePrefix + ";");
		fout << linePrefix << ";" << comment << "\n";
	}

	void WriteBlankLine()
	{
		fout << "\n";
	}

	void WriteHorizontalLine()
	{
		fout <<
			";-------------------------------------------------------------------------------\n";
	}

	void WriteSectionHeader(const char * section)
	{
		fout << "\n";
		WriteHorizontalLine();
		WriteComment(section);
		WriteHorizontalLine();
	}

	void WritePushScope(char begin = '{', char end = '}')
	{
		fout << linePrefix << begin << "\n";
		linePrefix += "\t";
		closingScope += end;
	}

	void WritePushScopeStruct()
	{
		WritePushScope('[', ']');
	}

	void WritePopScope()
	{
		assert(!linePrefix.empty());
		linePrefix.resize(linePrefix.size() - 1);

		fout << linePrefix << closingScope[closingScope.size() - 1] <<
			"\n";

		closingScope.resize(closingScope.size() - 1);
	}

	void WriteVariable(const std::string& key, const std::string& value,
		const std::string& operation = "=")
	{
		fout << linePrefix << "." <<
			key << " " << operation << " " << value << "\n";
	}

	void WriteCommand(const std::string& command, const std::string& value = std::string())
	{
		fout << linePrefix << command;
		if (!value.empty())
		{
			fout << "(" << value << ")";
		}
		fout << "\n";
	}

	void WriteArray(const std::string& key,
		const std::vector<std::string>& values,
		const std::string& operation = "=")
	{
		WriteVariable(key, "", operation);
		WritePushScope();
		size_t size = values.size();
		for (size_t index = 0; index < size; ++index)
		{
			const std::string & value = values[index];
			bool isLast = index == size - 1;

			fout << linePrefix << value;
			if (!isLast)
			{
				fout << ',';
			}
			fout << "\n";
		}
		WritePopScope();
	}

private:
	cmGeneratedFileStream & fout;
	std::string linePrefix;
	std::string closingScope;
};

//----------------------------------------------------------------------------
class cmGlobalFastbuildGenerator::Detail::Detection
{
public:
	static std::string GetLastFolderName(const std::string& string)
	{
		return string.substr(string.rfind('/')+1);
	}

#define FASTBUILD_DOLLAR_TAG "FASTBUILD_DOLLAR_TAG"

	static void UnescapeFastbuildVariables(std::string& string)
	{
		// Unescape the Fastbuild configName symbol with $
		cmSystemTools::ReplaceString(string, "^", "^^");
		cmSystemTools::ReplaceString(string, "$$", "^$");
		cmSystemTools::ReplaceString(string, FASTBUILD_DOLLAR_TAG, "$");
		//cmSystemTools::ReplaceString(string, "$$ConfigName$$", "$ConfigName$");
		//cmSystemTools::ReplaceString(string, "^$ConfigName^$", "$ConfigName$");
	}

	static void ResolveFastbuildVariables(std::string& string, const std::string& configName)
	{
		// Replace Fastbuild configName with the config name
		cmSystemTools::ReplaceString(string, "$ConfigName$", configName);
	}

	static std::string BuildCommandLine(
		const std::vector<std::string> &cmdLines)
	{
#ifdef _WIN32
		const char * cmdExe = "cmd.exe";
		std::string cmdExeAbsolutePath = cmSystemTools::FindProgram(cmdExe);
#endif

		// If we have no commands but we need to build a command anyway, use ":".
		// This happens when building a POST_BUILD value for link targets that
		// don't use POST_BUILD.
		if (cmdLines.empty())
		{
#ifdef _WIN32
			return cmdExeAbsolutePath + " /C \"cd .\"";
#else
			return ":";
#endif
		}

		std::ostringstream cmd;
		for (std::vector<std::string>::const_iterator li = cmdLines.begin();
			li != cmdLines.end(); ++li)
#ifdef _WIN32
		{
			if (li != cmdLines.begin())
			{
				cmd << " && ";
			}
			else if (cmdLines.size() > 1)
			{
				cmd << cmdExeAbsolutePath << " /C \"";
			}
			cmd << *li;
		}
		if (cmdLines.size() > 1)
		{
			cmd << "\"";
		}
#else
		{
			if (li != cmdLines.begin())
			{
				cmd << " && ";
			}
			cmd << *li;
		}
#endif
		std::string cmdOut = cmd.str();
		UnescapeFastbuildVariables(cmdOut);

		return cmdOut;
	}

	static void DetectConfigurations(cmGlobalFastbuildGenerator * self,
		cmMakefile* mf,
		std::vector<std::string> & configurations, std::map<std::string,std::string>& configAlias)
	{
		// process the configurations
		const char* configList =
			self->GetCMakeInstance()->GetCacheDefinition("CMAKE_CONFIGURATION_TYPES");
		if (configList)
		{
			std::vector<std::string> argsOut;
			cmSystemTools::ExpandListArgument(configList, argsOut);
			for (std::vector<std::string>::iterator iter = argsOut.begin();
				iter != argsOut.end(); ++iter)
			{
				if (std::find(configurations.begin(), configurations.end(), *iter) == configurations.end())
				{
					configurations.push_back(*iter);
				}
			}
		}

		// default to at least Debug and Release
		if (configurations.size() == 0)
		{
			configurations.push_back("Debug");
			configurations.push_back("Release");
			configurations.push_back("MinSizeRel");
			configurations.push_back("RelWithDebInfo");
		}

		// Reset the entry to have a semi-colon separated list.
		std::string configs = configurations[0];
		for (unsigned int i = 1; i < configurations.size(); ++i)
		{
			configs += ";";
			configs += configurations[i];
		}

        // Find aliases for generating visual studio solutions
        configAlias.clear();
        for (unsigned int i = 0; i < configurations.size(); ++i)
        {
            const std::string& s = configurations[i];
            if (s == "Debug")
                configAlias[s] = VSDebugConfiguration;
            else if (s == "RelWithDebInfo")
                configAlias[s] = VSRelWithDebInfoConfiguration;
        }

		// Add a cache definition
		mf->AddCacheDefinition(
			"CMAKE_CONFIGURATION_TYPES",
			configs.c_str(),
			"Semicolon separated list of supported configuration types, "
			"only supports Debug, Release, MinSizeRel, and RelWithDebInfo, "
			"anything else will be ignored.",
			cmState::STRING);
	}

	struct FastbuildTargetNames
	{
		std::string targetNameOut;
		std::string targetNameReal;
		std::string targetNameImport;
		std::string targetNamePDB;
		std::string targetNameSO;
	
		std::string targetOutput;
		std::string targetOutputReal;
		std::string targetOutputImplib;
		std::string targetOutputDir;
		std::string targetOutputPDBDir;
		std::string targetOutputCompilePDBDir;
	};

	static void DetectOutput(
		FastbuildTargetNames & targetNamesOut,
		cmGeneratorTarget &target,
		const std::string & configName)
	{
		if (target.GetType() == cmState::EXECUTABLE)
		{
			target.GetExecutableNames(
				targetNamesOut.targetNameOut,
				targetNamesOut.targetNameReal,
				targetNamesOut.targetNameImport,
				targetNamesOut.targetNamePDB,
				configName);
		}
		else
		{
			target.GetLibraryNames(
				targetNamesOut.targetNameOut,
				targetNamesOut.targetNameSO,
				targetNamesOut.targetNameReal,
				targetNamesOut.targetNameImport,
				targetNamesOut.targetNamePDB,
				configName);
		}
		
		if (target.HaveWellDefinedOutputFiles())
		{
			targetNamesOut.targetOutputDir = target.GetDirectory(configName) + "/";

			targetNamesOut.targetOutput = target.GetFullPath(configName);
			targetNamesOut.targetOutputReal = target.GetFullPath(configName,
				/*implib=*/false,
				/*realpath=*/true);
			targetNamesOut.targetOutputImplib = target.GetFullPath(configName,
				/*implib=*/true);
		}
		else
		{
			targetNamesOut.targetOutputDir = target.Target->GetMakefile()->GetCurrentBinaryDirectory();
			if (targetNamesOut.targetOutputDir.empty() || 
				targetNamesOut.targetOutputDir == ".")
			{
				targetNamesOut.targetOutputDir = target.GetName();
			}
			else 
			{
				targetNamesOut.targetOutputDir += "/";
				targetNamesOut.targetOutputDir += target.GetName();
			}
			targetNamesOut.targetOutputDir += "/";
			targetNamesOut.targetOutputDir += configName;
			targetNamesOut.targetOutputDir += "/";

			targetNamesOut.targetOutput = targetNamesOut.targetOutputDir + "/" +
				targetNamesOut.targetNameOut;
			targetNamesOut.targetOutputImplib = targetNamesOut.targetOutputDir + "/" +
				targetNamesOut.targetNameImport;
			targetNamesOut.targetOutputReal = targetNamesOut.targetOutputDir + "/" +
				targetNamesOut.targetNameReal;
		}

		if (target.GetType() == cmState::EXECUTABLE ||
			target.GetType() == cmState::STATIC_LIBRARY ||
			target.GetType() == cmState::SHARED_LIBRARY ||
			target.GetType() == cmState::MODULE_LIBRARY)
		{
			targetNamesOut.targetOutputPDBDir = target.GetPDBDirectory(configName);
			targetNamesOut.targetOutputPDBDir += "/";
		}
		if (target.GetType() <= cmState::OBJECT_LIBRARY)
		{
			targetNamesOut.targetOutputCompilePDBDir = target.GetCompilePDBPath(configName);
			if (targetNamesOut.targetOutputCompilePDBDir.empty())
			{
				targetNamesOut.targetOutputCompilePDBDir = target.GetSupportDirectory() + "/" + configName + "/";
			}
		}

		// Make sure all obey the correct slashes
		cmSystemTools::ConvertToOutputSlashes(targetNamesOut.targetOutput);
		cmSystemTools::ConvertToOutputSlashes(targetNamesOut.targetOutputImplib);
		cmSystemTools::ConvertToOutputSlashes(targetNamesOut.targetOutputReal);
		cmSystemTools::ConvertToOutputSlashes(targetNamesOut.targetOutputDir);
		cmSystemTools::ConvertToOutputSlashes(targetNamesOut.targetOutputPDBDir);
		cmSystemTools::ConvertToOutputSlashes(targetNamesOut.targetOutputCompilePDBDir);
	}

	static void ComputeLinkCmds(std::vector<std::string> & linkCmds,
		cmLocalFastbuildGenerator *lg,
		cmGeneratorTarget &target,
		std::string configName)
	{
		const std::string& linkLanguage = target.GetLinkerLanguage(configName);
		cmMakefile* mf = lg->GetMakefile();
		{
			std::string linkCmdVar = 
				target.GetCreateRuleVariable(linkLanguage, configName);
			const char *linkCmd = mf->GetDefinition(linkCmdVar);
			if (linkCmd)
			{
				cmSystemTools::ExpandListArgument(linkCmd, linkCmds);
				return;
			}
		}

		// If the above failed, then lets try this:
		switch (target.GetType()) 
		{
			case cmState::STATIC_LIBRARY: 
			{
				// We have archive link commands set. First, delete the existing archive.
				{
					std::string cmakeCommand = lg->ConvertToOutputFormat(
						mf->GetRequiredDefinition("CMAKE_COMMAND"),
						cmLocalGenerator::SHELL);
					linkCmds.push_back(cmakeCommand + " -E remove $TARGET_FILE");
				}
				// TODO: Use ARCHIVE_APPEND for archives over a certain size.
				{
					std::string linkCmdVar = "CMAKE_";
					linkCmdVar += linkLanguage;
					linkCmdVar += "_ARCHIVE_CREATE";
					const char *linkCmd = mf->GetRequiredDefinition(linkCmdVar);
					cmSystemTools::ExpandListArgument(linkCmd, linkCmds);
				}
				{
					std::string linkCmdVar = "CMAKE_";
					linkCmdVar += linkLanguage;
					linkCmdVar += "_ARCHIVE_FINISH";
					const char *linkCmd = mf->GetRequiredDefinition(linkCmdVar);
					cmSystemTools::ExpandListArgument(linkCmd, linkCmds);
				}
				return;
			}
			case cmState::SHARED_LIBRARY:
			case cmState::MODULE_LIBRARY:
			case cmState::EXECUTABLE:
				break;
			default:
				assert(0 && "Unexpected target type");
		}
		return;
	}

	static std::string ComputeDefines(
		cmLocalFastbuildGenerator *lg,
		cmGeneratorTarget &target,
		const cmSourceFile* source,
		const std::string& configName,
		const std::string& language)
	{
		std::set<std::string> defines;

		// Add the export symbol definition for shared library objects.
		if(const char* exportMacro = target.GetExportMacro())
		{
			lg->AppendDefines(defines, exportMacro);
		}

		// Add preprocessor definitions for this target and configuration.
		lg->AddCompileDefinitions(defines, &target,
			configName, language);

		if (source)
		{
			lg->AppendDefines(defines,
				source->GetProperty("COMPILE_DEFINITIONS"));

			std::string defPropName = "COMPILE_DEFINITIONS_";
			defPropName += cmSystemTools::UpperCase(configName);
			lg->AppendDefines(defines,
				source->GetProperty(defPropName));
		}

		// Add a definition for the configuration name.
		// NOTE: CMAKE_TEST_REQUIREMENT The following was added specifically to 
		// facillitate cmake testing. Doesn't feel right to do this...
		std::string configDefine = "CMAKE_INTDIR=\"";
		configDefine += configName;
		configDefine += "\"";
		lg->AppendDefines(defines, configDefine);

		std::string definesString;
		lg->JoinDefines(defines, definesString,
			language);

		return definesString;
	}

	static void DetectLinkerLibPaths(
		std::string& linkerLibPath,
		cmLocalFastbuildGenerator *lg,
		cmGeneratorTarget &target,
		const std::string & configName )
	{
		cmMakefile* pMakefile = lg->GetMakefile();
		cmComputeLinkInformation* pcli = target.GetLinkInformation(configName);
		if (!pcli)
		{
			// No link information, then no linker library paths
			return;
		}
		cmComputeLinkInformation& cli = *pcli;

		std::string libPathFlag =
			pMakefile->GetRequiredDefinition("CMAKE_LIBRARY_PATH_FLAG");
		std::string libPathTerminator =
			pMakefile->GetSafeDefinition("CMAKE_LIBRARY_PATH_TERMINATOR");

		// Append the library search path flags.
		std::vector<std::string> const& libDirs = cli.GetDirectories();
		for (std::vector<std::string>::const_iterator libDir = libDirs.begin();
			libDir != libDirs.end(); ++libDir)
		{
			std::string libpath = lg->ConvertToOutputForExisting(*libDir,
				cmLocalGenerator::START_OUTPUT,
				cmLocalGenerator::SHELL);
			cmSystemTools::ConvertToOutputSlashes(libpath);

			// Add the linker lib path twice, once raw, then once with
			// the configname attached
			std::string configlibpath = libpath + "/" + configName;
			cmSystemTools::ConvertToOutputSlashes(configlibpath);

			linkerLibPath += " " + libPathFlag;
			linkerLibPath += libpath;
			linkerLibPath += libPathTerminator;

			linkerLibPath += " " + libPathFlag;
			linkerLibPath += configlibpath;
			linkerLibPath += libPathTerminator;
			linkerLibPath += " ";
		}
	}

	static bool DetectBaseLinkerCommand(std::string & command,
		cmLocalFastbuildGenerator *lg,
		cmGeneratorTarget &target,
		const std::string & configName)
	{
		const std::string& linkLanguage = target.GetLinkerLanguage(configName);
		if (linkLanguage.empty()) {
			cmSystemTools::Error("CMake can not determine linker language for "
				"target: ",
				target.GetName().c_str());
			return false;
		}

		cmLocalGenerator::RuleVariables vars;
		vars.RuleLauncher = "RULE_LAUNCH_LINK";
		vars.CMTarget = &target;
		vars.Language = linkLanguage.c_str();

		std::string responseFlag;
		vars.Objects = FASTBUILD_DOLLAR_TAG "FB_INPUT_1_PLACEHOLDER" FASTBUILD_DOLLAR_TAG;
		vars.LinkLibraries = "";
		
		vars.ObjectDir = FASTBUILD_DOLLAR_TAG "TargetOutCompilePDBDir" FASTBUILD_DOLLAR_TAG;
		vars.Target = FASTBUILD_DOLLAR_TAG "FB_INPUT_2_PLACEHOLDER" FASTBUILD_DOLLAR_TAG;

		vars.TargetSOName = FASTBUILD_DOLLAR_TAG"TargetOutSO" FASTBUILD_DOLLAR_TAG;
		vars.TargetPDB = FASTBUILD_DOLLAR_TAG "TargetOutPDBDir" FASTBUILD_DOLLAR_TAG FASTBUILD_DOLLAR_TAG "TargetNamePDB" FASTBUILD_DOLLAR_TAG;

		// Setup the target version.
		std::string targetVersionMajor;
		std::string targetVersionMinor;
		{
			std::ostringstream majorStream;
			std::ostringstream minorStream;
			int major;
			int minor;
			target.GetTargetVersion(major, minor);
			majorStream << major;
			minorStream << minor;
			targetVersionMajor = majorStream.str();
			targetVersionMinor = minorStream.str();
		}
		vars.TargetVersionMajor = targetVersionMajor.c_str();
		vars.TargetVersionMinor = targetVersionMinor.c_str();

		vars.Defines = FASTBUILD_DOLLAR_TAG "CompileDefineFlags" FASTBUILD_DOLLAR_TAG;
		vars.Flags = FASTBUILD_DOLLAR_TAG "TargetFlags" FASTBUILD_DOLLAR_TAG;
		vars.LinkFlags = FASTBUILD_DOLLAR_TAG "LinkFlags" FASTBUILD_DOLLAR_TAG " " FASTBUILD_DOLLAR_TAG "LinkPath" FASTBUILD_DOLLAR_TAG;

		// compute MANIFESTS
		std::vector<cmSourceFile const*> manifest_srcs;
		target.GetManifests(manifest_srcs, configName);
		std::vector<std::string> manifests;
		for (std::vector<cmSourceFile const*>::iterator mi = manifest_srcs.begin();
			mi != manifest_srcs.end(); ++mi) {
			manifests.push_back(lg->Convert((*mi)->GetFullPath(), cmOutputConverter::HOME_OUTPUT, cmOutputConverter::SHELL));
		}
		std::string manifestsStr = cmJoin(manifests, " ");
		vars.Manifests = manifestsStr.c_str();

		// Rule for linking library/executable.
		std::vector<std::string> linkCmds;
		ComputeLinkCmds(linkCmds, lg, target, configName);
		for (std::vector<std::string>::iterator i = linkCmds.begin();
			i != linkCmds.end();
			++i)
		{
			lg->ExpandRuleVariables(*i, vars);
		}
		
		command = BuildCommandLine(linkCmds);

		return true;
	}

	static void SplitExecutableAndFlags(const std::string & command,
		std::string & executable, std::string & options)
	{
		// Remove the command from the front
		std::vector<std::string> args = cmSystemTools::ParseArguments(command.c_str());

		// Join the args together and remove 0 from the front
		std::stringstream argSet;
		std::copy(args.begin() + 1, args.end(), std::ostream_iterator<std::string>(argSet, " "));
		
		executable = args[0];
		options = argSet.str();
	}

	static void DetectBaseCompileCommand(std::string & command,
		cmLocalFastbuildGenerator *lg,
		cmGeneratorTarget &target,
		const std::string & language,
		const std::string & configName)
	{
		cmLocalGenerator::RuleVariables compileObjectVars;
		compileObjectVars.CMTarget = &target;
		compileObjectVars.Language = language.c_str();
		compileObjectVars.Source = FASTBUILD_DOLLAR_TAG "FB_INPUT_1_PLACEHOLDER" FASTBUILD_DOLLAR_TAG;
		compileObjectVars.Object = FASTBUILD_DOLLAR_TAG "FB_INPUT_2_PLACEHOLDER" FASTBUILD_DOLLAR_TAG;
		compileObjectVars.ObjectDir = FASTBUILD_DOLLAR_TAG "TargetOutCompilePDBDir" FASTBUILD_DOLLAR_TAG;
		compileObjectVars.ObjectFileDir = "";
		compileObjectVars.Flags = "";
		compileObjectVars.Defines = "";

		// Compute INCLUDES
		std::vector<std::string> includes;
		lg->GetIncludeDirectories(includes, &target,
			language, configName);
		// Add include directory flags.
		std::string includeFlags = lg->GetIncludeFlags(
			includes, &target, language,
			language == "RC" ? true : false, // full include paths for RC
			                                 // needed by cmcldeps
			false, configName);
		//if (lg->GetGlobalGenerator()->IsGCCOnWindows())
		//    std::replace(includeFlags.begin(), includeFlags.end(), '\\', '/');
		compileObjectVars.Includes = includeFlags.c_str();

		compileObjectVars.TargetCompilePDB = FASTBUILD_DOLLAR_TAG "TargetOutCompilePDBDir" FASTBUILD_DOLLAR_TAG FASTBUILD_DOLLAR_TAG "TargetNamePDB" FASTBUILD_DOLLAR_TAG;

		// Rule for compiling object file.
		std::string compileCmdVar = "CMAKE_";
		compileCmdVar += language;
		compileCmdVar += "_COMPILE_OBJECT";
		std::string compileCmd = lg->GetMakefile()->GetRequiredDefinition(compileCmdVar);
		std::vector<std::string> compileCmds;
		cmSystemTools::ExpandListArgument(compileCmd, compileCmds);

		for (std::vector<std::string>::iterator i = compileCmds.begin();
			i != compileCmds.end(); ++i)
		{
			std::string & compileCmdStr = *i;
			lg->ExpandRuleVariables(compileCmdStr, compileObjectVars);
		}

		command = BuildCommandLine(compileCmds);
	}

	static std::string DetectCompileRule(cmLocalFastbuildGenerator *lg,
		cmGeneratorTarget &target,
		const std::string& lang)
	{
		cmLocalGenerator::RuleVariables vars;
		vars.RuleLauncher = "RULE_LAUNCH_COMPILE";
		vars.CMTarget = &target;
		vars.Language = lang.c_str();
		vars.Source = "$in";
		vars.Object = "$out";
		vars.Defines = "$DEFINES";
		vars.TargetPDB = "$TARGET_PDB";
		vars.TargetCompilePDB = "$TARGET_COMPILE_PDB";
		vars.ObjectDir = "$OBJECT_DIR";
		vars.ObjectFileDir = "$OBJECT_FILE_DIR";
		vars.Flags = "$FLAGS";

		cmMakefile* mf = lg->GetMakefile();

		// Rule for compiling object file.
		const std::string cmdVar = std::string("CMAKE_") + lang + "_COMPILE_OBJECT";
		std::string compileCmd = mf->GetRequiredDefinition(cmdVar);
		std::vector<std::string> compileCmds;
		cmSystemTools::ExpandListArgument(compileCmd, compileCmds);

		for (std::vector<std::string>::iterator i = compileCmds.begin();
			i != compileCmds.end(); ++i)
		{
			std::string & compileCmdStr = *i;
			lg->ExpandRuleVariables(compileCmdStr, vars);
		}

		std::string cmdLine =
			BuildCommandLine(compileCmds);

		return cmdLine;
	}

	static void DetectLanguages(std::set<std::string> & languages,
		cmGlobalFastbuildGenerator * self,
		cmGeneratorTarget &target)
	{
		// Object libraries do not have linker stages
		// nor utilities
		bool hasObjectGroups =
			target.GetType() != cmState::UTILITY &&
			target.GetType() != cmState::GLOBAL_TARGET;
		if (!hasObjectGroups)
		{
			return;
		}

		std::vector<std::string>::const_iterator
			iter = self->GetConfigurations().begin(),
			end = self->GetConfigurations().end();
		for (; iter != end; ++iter)
		{
			const std::string & configName = *iter;

			std::vector<const cmSourceFile*> sourceFiles;
			target.GetObjectSources(sourceFiles, configName);
			for (std::vector<const cmSourceFile*>::const_iterator
				i = sourceFiles.begin(); i != sourceFiles.end(); ++i)
			{
				const std::string& lang = (*i)->GetLanguage();
				if (!lang.empty())
				{
					languages.insert(lang);
				}
			}
		}
	}

	static void FilterSourceFiles(std::vector<cmSourceFile const*> & filteredSourceFiles,
		std::vector<cmSourceFile const*> & sourceFiles, const std::string & language)
	{
		for (std::vector<cmSourceFile const*>::const_iterator
			i = sourceFiles.begin(); i != sourceFiles.end(); ++i)
		{
			const cmSourceFile* sf = *i;
			if (sf->GetLanguage() == language)
			{
				filteredSourceFiles.push_back(sf);
			}
		}
	}

	static void DetectCompilerFlags(std::string & compileFlags,
		cmLocalFastbuildGenerator *lg,
		cmGeneratorTarget &target,
		const cmSourceFile* source,
		const std::string& language,
		const std::string& configName)
	{
		lg->AddLanguageFlags(compileFlags, 
			language, 
			configName);

		lg->AddArchitectureFlags(compileFlags,
			&target,
			language,
			configName);

		// Add shared-library flags if needed.
		lg->AddCMP0018Flags(compileFlags, 
			&target,
			language,
			configName);

		lg->AddVisibilityPresetFlags(compileFlags, &target,
			language);

		// Append old-style preprocessor definition flags.
		lg->AppendFlags(compileFlags,
			lg->GetMakefile()->GetDefineFlags());

		// Add target-specific flags.
		lg->AddCompileOptions(compileFlags, 
			&target,
			language,
			configName);

		if (source)
		{
			lg->AppendFlags(compileFlags, source->GetProperty("COMPILE_FLAGS"));
		}
	}

	static void DetectTargetCompileDependencies(
		cmGlobalFastbuildGenerator* gg,
		cmGeneratorTarget& target,
		std::vector<std::string>& dependencies,
		std::vector<std::string>& linkDependencies)
	{
		if (target.GetType() == cmState::GLOBAL_TARGET)
		{
			// Global targets only depend on other utilities, which may not appear in
			// the TargetDepends set (e.g. "all").
			std::set<std::string> const& utils = target.GetUtilities();
			std::copy(utils.begin(), utils.end(), std::back_inserter(dependencies));
		}
		else 
		{
			cmTargetDependSet const& targetDeps =
				gg->GetTargetDirectDepends(&target);
			for (cmTargetDependSet::const_iterator i = targetDeps.begin();
				i != targetDeps.end(); ++i)
			{
				const cmTargetDepend& depTarget = *i;
				if (depTarget->GetType() == cmState::INTERFACE_LIBRARY)
				{
					continue;
				}
				linkDependencies.push_back(depTarget->GetName());
			}
		}
		// first sort entries by name, to make it deterministic between runs
		// TODO: this should actually be fixed on CMake generic sorting side,
		// which should not rely on pointer values to sort and iterate
		std::sort(dependencies.begin(), dependencies.end());
		std::sort(linkDependencies.begin(), linkDependencies.end());
	}
/*
	static void DetectTargetLinkDependencies(
		cmGeneratorTarget& target,
		const std::string& configName,
		std::vector<std::string>& dependencies)
	{
		// Static libraries never depend on other targets for linking.
		if (target.GetType() == cmState::STATIC_LIBRARY ||
			target.GetType() == cmState::OBJECT_LIBRARY)
		{
			return;
		}

		cmComputeLinkInformation* cli =
			target.GetLinkInformation(configName);
		if (!cli)
		{
			return;
		}

		const std::vector<std::string> &deps = cli->GetDepends();
		std::copy(deps.begin(), deps.end(), std::back_inserter(dependencies));
	}
*/
	static void DetectTargetLinkOtherInputs(
		cmLocalFastbuildGenerator *lg,
		cmGeneratorTarget& target,
		const std::string& configName,
		std::vector<std::string>& dependencies)
	{
		// compute MANIFESTS
		std::vector<cmSourceFile const*> manifest_srcs;
		target.GetManifests(manifest_srcs, configName);
		for (std::vector<cmSourceFile const*>::iterator mi = manifest_srcs.begin();
			mi != manifest_srcs.end(); ++mi) {
			dependencies.push_back(lg->Convert((*mi)->GetFullPath(), cmOutputConverter::HOME_OUTPUT, cmOutputConverter::SHELL));
		}
	}

	static void DetectTargetLinkItems(
		cmGeneratorTarget& target,
		const std::string& configName,
		std::vector<std::string>& libs,
		std::ostringstream& log
		)
	{
		// Static libraries never depend on other targets for linking.
		if (target.GetType() == cmState::STATIC_LIBRARY ||
			target.GetType() == cmState::OBJECT_LIBRARY)
		{
			return;
		}

		cmComputeLinkInformation* cli =
			target.GetLinkInformation(configName);
		if (!cli)
		{
			return;
		}

		const cmComputeLinkInformation::ItemVector &items = cli->GetItems();
		for (cmComputeLinkInformation::ItemVector::const_iterator li = items.begin(); li != items.end();
			++li) {
			log << " - " << li->Value;
			if (li->IsPath) log << " (Path)";
			if (li->Target) log << " (Target " << li->Target->GetName() << " " << cmState::GetTargetTypeName(li->Target->GetType()) << ")";
			log << "\n";
			if (li->Target && li->Target->GetType() == cmState::INTERFACE_LIBRARY) {
				continue;
			}
			if (li->IsPath || li->Target) {
				libs.push_back(li->Value); // += this->ConvertToLinkReference(li->Value, shellFormat);
			}
			//else {
			//	libs.push_back(li->Value);
			//}
		}
	}

	static std::string DetectTargetCompileOutputDir(
		cmLocalFastbuildGenerator* lg,
		const cmGeneratorTarget& target,
		std::string configName)
	{
		std::string result = lg->GetTargetDirectory(&target) + "/";
		if (!configName.empty())
		{
			result = result + configName + "/";
		}
		cmSystemTools::ConvertToOutputSlashes(result);
		return result;
	}

	static void DetectTargetObjectDependencies(
		cmGlobalFastbuildGenerator* gg,
		cmGeneratorTarget& target,
		const std::string& configName,
		std::vector<std::string>& dependencies)
	{
		// Iterate over all source files and look for 
		// object file dependencies
		std::set<std::string> objectLibs;

		std::vector<cmSourceFile*> sourceFiles;
		target.GetSourceFiles(sourceFiles, configName);
		for (std::vector<cmSourceFile*>::const_iterator
			i = sourceFiles.begin(); i != sourceFiles.end(); ++i)
		{
			const std::string& objectLib = (*i)->GetObjectLibrary();
			if (!objectLib.empty())
			{
				// Find the target this actually is (might be an alias)
				const cmTarget* objectTarget = gg->FindTarget(objectLib);
				if (objectTarget)
				{
					objectLibs.insert(objectTarget->GetName() + "-" + configName + "-products");
				}
			}
		}

		std::copy(objectLibs.begin(), objectLibs.end(),
			std::back_inserter(dependencies) );

		// Now add the external obj files that also need to be linked in
		std::vector<const cmSourceFile*> objFiles;
		target.GetExternalObjects(objFiles, configName);
		for (std::vector<const cmSourceFile*>::const_iterator
			i = objFiles.begin(); i != objFiles.end(); ++i)
		{
			const cmSourceFile* sourceFile = *i;
			if (sourceFile->GetObjectLibrary().empty())
			{
				dependencies.push_back(sourceFile->GetFullPath());
			}
		}
	}

	static void DetectCustomCommandOutputs(
		const cmCustomCommand* cc,
		cmGlobalFastbuildGenerator *gg,
		cmLocalFastbuildGenerator *lg,
		cmGeneratorTarget& target,
		const std::string& configName,
		std::vector<std::string>& fileOutputs,
		std::vector<std::string>& symbolicOutputs,
		bool& isConfigDependant,
		std::ostringstream* outDescription = NULL
	)
	{
		cmCustomCommandGenerator ccg(*cc, configName, lg);
		cmMakefile* makefile = lg->GetMakefile();
		const std::vector<std::string> &ccOutputs = ccg.GetOutputs();
		const std::vector<std::string> &byproducts = ccg.GetByproducts();
		std::vector<std::string> outputs;
		outputs.insert(outputs.end(), ccOutputs.begin(), ccOutputs.end());
		outputs.insert(outputs.end(), byproducts.begin(), byproducts.end());
		std::string workingDirectory = ccg.GetWorkingDirectory();
		if (workingDirectory.empty())
		{
			workingDirectory = makefile->GetCurrentBinaryDirectory();
		}
		if (workingDirectory.back() != '/')
		{
			workingDirectory += "/";
		}

		// Check if the outputs don't depend on the config name, and if we have a real output
		for (std::vector<std::string>::iterator iter = outputs.begin();
			iter != outputs.end();
			++iter)
		{
			std::string dep = *iter;
			cmSourceFile* depSourceFile = makefile->GetSource(dep); // GetSource called before any filtering of the file path
			bool isSymbolic = depSourceFile && depSourceFile->GetPropertyAsBool("SYMBOLIC");
			cmTarget* depTarget = gg->FindTarget(dep);
			if (depSourceFile && !depTarget)
				dep = depSourceFile->GetFullPath();
			else if (!depSourceFile && !depTarget)
			{
				if (!cmSystemTools::FileIsFullPath(dep.c_str()))
				{
					dep = workingDirectory + dep;
				}
			}
			Detection::UnescapeFastbuildVariables(dep);
			// Check if this file is symbolic and if it depends on config value (before resolving $ConfigName$)
			bool isConfigDep = dep.find("$ConfigName$") != std::string::npos;
			isConfigDependant = isConfigDependant || isConfigDep;
			if (outDescription) (*outDescription) << "        - " << dep << (depTarget ? " (Target)" : "") << (depSourceFile ? " (SourceFile)" : "") << (isSymbolic ? " (SYMBOLIC)" : "") << (isConfigDep ? " (CONFIG)" : "") << "\n";
			Detection::ResolveFastbuildVariables(dep, configName);
			if (!isSymbolic)
			{
				fileOutputs.push_back(dep);
			}
			else
			{
				symbolicOutputs.push_back(dep);
			}
		}
	}

	struct DependencySorter
	{
		struct TargetHelper
		{
			cmGlobalFastbuildGenerator *gg;

			std::string GetName(const cmGeneratorTarget* entry)
			{
				return entry->GetName();
			}

			void GetOutputs(const cmGeneratorTarget* entry, std::vector<std::string>& outputs)
			{
				outputs.push_back(entry->GetName());
			}

			void GetInputs(const cmGeneratorTarget* entry, std::vector<std::string>& inputs)
			{
				TargetDependSet const& ts = gg->GetTargetDirectDepends(entry);
				for (TargetDependSet::const_iterator iter = ts.begin(); iter != ts.end(); ++iter)
				{
					const cmGeneratorTarget * dtarget = *iter;
					inputs.push_back(dtarget->GetName());
				}
			}

			void AddDependency(const cmGeneratorTarget* /*from*/, const cmGeneratorTarget* /*to*/)
			{
			}
		};
		
		struct CustomCommandHelper
		{
			std::map<const cmSourceFile*, std::vector<std::string> > mapInputs;
			std::map<const cmSourceFile*, std::vector<std::string> > mapFileOutputs;
			std::map<const cmSourceFile*, std::vector<std::string> > mapSymbolicOutputs;
			std::map<const cmSourceFile*, std::vector<const cmSourceFile*> > mapInputCmds;
			std::map<const cmSourceFile*, std::string > mapWorkingDirectory;
			std::map<const cmSourceFile*, std::string > mapRuleName;
			std::vector<const cmSourceFile*> orderedCommands;

			std::string GetName(const cmSourceFile* entry)
			{
				return entry->GetFullPath();
			}

			void GetOutputs(const cmSourceFile* entry, std::vector<std::string>& outputs)
			{
				outputs = mapFileOutputs[entry];
				outputs.insert(outputs.end(), mapSymbolicOutputs[entry].begin(), mapSymbolicOutputs[entry].end());
			}

			void GetInputs(const cmSourceFile* entry, std::vector<std::string>& inputs)
			{
				inputs = mapInputs[entry];
			}

			void AddDependency(const cmSourceFile* from, const cmSourceFile* to)
			{
				mapInputCmds[from].push_back(to);
			}
		};

		template <class TType, class TTypeHelper>
		struct SorterFunctor
		{
			TTypeHelper* helper;
			SorterFunctor(TTypeHelper* h) : helper(h) {}
			int operator()(const TType* va, const TType* vb)
			{
				return helper->GetName(va) < helper->GetName(vb);
			}
		};

		template <class TType, class TTypeHelper>
		static void Sort(TTypeHelper& helper, std::vector<const TType*>& entries)
		{
			typedef unsigned int EntryIndex;
			typedef std::vector<std::string> StringVector;
			typedef std::vector<EntryIndex> OrderedEntrySet;
			typedef std::map<std::string, EntryIndex> OutputMap;

			// first sort entries by name, to make it deterministic between runs
			// TODO: this should actually be fixed on CMake generic sorting side,
			// which should not rely on pointer values to sort and iterate
			std::sort(entries.begin(), entries.end(), SorterFunctor<TType,TTypeHelper>(&helper));

			// Build up a map of outputNames to entries
			OutputMap outputMap;
			for (EntryIndex index = 0; index < entries.size(); ++index)
			{
				const TType* entry = entries[index];
				StringVector outputs;
				helper.GetOutputs(entry, outputs);

				for (StringVector::iterator outIter = outputs.begin();
					outIter != outputs.end();
					++outIter)
				{
					outputMap[*outIter] = index;
				}
			}

			// Now build a forward and reverse map of dependencies
			// Build the reverse graph, 
			// each target, and the set of things that depend upon it
			typedef std::map<EntryIndex, std::vector<EntryIndex> > DepMap;
			DepMap forwardDeps;
			DepMap reverseDeps;
			for (EntryIndex index = 0; index < entries.size(); ++index)
			{
				const TType* entry = entries[index];
				std::vector<EntryIndex>& entryInputs = forwardDeps[index];

				StringVector inputs;
				helper.GetInputs(entry, inputs);
				for (StringVector::const_iterator inIter = inputs.begin(); 
					inIter != inputs.end(); 
					++inIter)
				{
					const std::string& input = *inIter;
					// Lookup the input in the output map and find the right entry
					typename OutputMap::iterator findResult = outputMap.find(input);
					if (findResult != outputMap.end())
					{
						const EntryIndex dentryIndex = findResult->second;
						if (std::find(entryInputs.begin(), entryInputs.end(), dentryIndex) == entryInputs.end())
						{
							entryInputs.push_back(dentryIndex);
							reverseDeps[dentryIndex].push_back(index);
							helper.AddDependency(entries[index], entries[dentryIndex]);
						}
					}
				}
			}

			// We have all the information now.
			// Clear the array passed in
			std::vector<const TType*> sortedEntries;

			// Now iterate over each target with its list of dependencies.
			// And dump out ones that have 0 dependencies.
			bool written = true;
			while (!forwardDeps.empty() && written)
			{
				written = false;
				for (typename DepMap::iterator iter = forwardDeps.begin();
					iter != forwardDeps.end(); ++iter)
				{
					std::vector<EntryIndex>& fwdDeps = iter->second;
					EntryIndex index = iter->first;
					const TType* entry = entries[index];
					if (!fwdDeps.empty())
					{
						// Looking for empty dependency lists.
						// Those are the next to be written out
						continue;
					}

					// dependency list is empty,
					// add it to the output list
					written = true;
					sortedEntries.push_back(entry);

					// Use reverse dependencies to determine 
					// what forward dep lists to adjust
					std::vector<EntryIndex>& revDeps = reverseDeps[index];
					for (unsigned int i = 0; i < revDeps.size(); ++i)
					{
						EntryIndex revDepIndex = revDeps[i];
						const TType* revDep = entries[revDepIndex];

						// Fetch the list of deps on that target
						std::vector<EntryIndex>& revDepFwdDeps =
							forwardDeps[revDepIndex];
						// remove the one we just added from the list
						revDepFwdDeps.erase(
							std::remove(revDepFwdDeps.begin(), revDepFwdDeps.end(), index),
							revDepFwdDeps.end());
					}

					// Remove it from forward deps so not
					// considered again
					forwardDeps.erase(index);

					// Must break now as we've invalidated
					// our forward deps iterator
					break;
				}
			}

			// Validation...
			// Make sure we managed to find a place
			// to insert every dependency.
			// If this fires, then there is most likely
			// a cycle in the graph...
			assert(forwardDeps.empty());

			// Swap entries and sortedEntries to return the result
			entries.swap(sortedEntries);
		}
	};

	typedef std::vector<const cmGeneratorTarget*> OrderedTargetSet;
	static void ComputeTargetOrderAndDependencies(
		cmGlobalFastbuildGenerator* gg,
		OrderedTargetSet& orderedTargets)
	{
		TargetDependSet projectTargets;
		TargetDependSet originalTargets;
		std::map<std::string, std::vector<cmLocalGenerator*> >::const_iterator
			it = gg->GetProjectMap().begin(),
			end = gg->GetProjectMap().end();
		for(; it != end; ++it)
		{
			const std::vector<cmLocalGenerator*>& generators = it->second;
			cmLocalFastbuildGenerator* root =
				static_cast<cmLocalFastbuildGenerator*>(generators[0]);
			
			// Given this information, calculate the dependencies:
			// Collect all targets under this root generator and the transitive
			// closure of their dependencies.
			
			gg->GetTargetSets(projectTargets, originalTargets, root, generators);
		}

		// Iterate over the targets and export their order
		for (TargetDependSet::iterator iter = projectTargets.begin();
			iter != projectTargets.end();
			++iter)
		{
			const cmTargetDepend& targetDepend = *iter;
			const cmGeneratorTarget* target = targetDepend;

			orderedTargets.push_back(target);
		}

		DependencySorter::TargetHelper targetHelper = {gg};
		DependencySorter::Sort(targetHelper, orderedTargets);
	}

	// Iterate over all targets and remove the ones that are
	// not needed for generation.
	// i.e. the nested global targets
	struct RemovalTest
	{
		bool operator()(const cmGeneratorTarget* target) const
		{
			if (target->GetType() == cmState::GLOBAL_TARGET)
			{
				// We only want to process global targets that live in the home
				// (i.e. top-level) directory.  CMake creates copies of these targets
				// in every directory, which we don't need.
				cmMakefile *mf = target->Target->GetMakefile();
				/// @TODO: not sure if this works, add logs to check compared values
				/// -> yes it works (checked by the enabling message below)
				if (strcmp(mf->GetCurrentSourceDirectory(), mf->GetCMakeInstance()->GetHomeDirectory()) != 0)
				{
					//std::ostringstream s;
					//s << "Ignoring global target " << target->GetName() << " as it is not from the top-level directory: " << mf->GetCurrentSourceDirectory() << " != " << mf->GetCMakeInstance()->GetHomeDirectory() << std::endl;
					//cmSystemTools::Message(s.str().c_str());
					return true;
				}
			}
			return false;
		}
	};

	static void StripNestedGlobalTargets( OrderedTargetSet& orderedTargets )
	{
		orderedTargets.erase(
			std::remove_if(orderedTargets.begin(), orderedTargets.end(), RemovalTest()),
			orderedTargets.end());
	}

	static void DetectCompilerExtraFiles(const std::string& compilerID,
		const std::string& version, std::vector<std::string>& extraFiles)
	{
		// Output a list of files that are relative to $CompilerRoot$
		//return;

		if (compilerID == "MSVC")
		{
			if (version.compare(0, 3, "19.") != std::string::npos)
			{
				// Using vs2015
				const char *vs2013_extraFiles[12] = {
					"$CompilerRoot$\\c1.dll",
					"$CompilerRoot$\\c1xx.dll",
					"$CompilerRoot$\\c2.dll",
					"$CompilerRoot$\\msobj140.dll",
					"$CompilerRoot$\\mspdb140.dll",
					"$CompilerRoot$\\mspdbsrv.exe", // not sure this one makes sense...
					"$CompilerRoot$\\mspdbcore.dll",
					"$CompilerRoot$\\mspft140.dll",
					"$CompilerRoot$\\1033\\clui.dll",
					"$CompilerRoot$\\..\\redist\\x86\\Microsoft.VC140.CRT\\msvcp140.dll",
					"$CompilerRoot$\\..\\redist\\x86\\Microsoft.VC140.CRT\\vcruntime140.dll",
					"$CompilerRoot$\\..\\redist\\x86\\Microsoft.VC140.CRT\\vccorlib140.dll"
				};
				extraFiles.insert(extraFiles.end(), &vs2013_extraFiles[0], &vs2013_extraFiles[12]);
			}
			else if (version.compare(0, 3, "18.") != std::string::npos)
			{
				// Using vs2013
				const char *vs2013_extraFiles[13] = {
					"$CompilerRoot$\\c1.dll",
					"$CompilerRoot$\\c1ast.dll",
					"$CompilerRoot$\\c1xx.dll",
					"$CompilerRoot$\\c1xxast.dll",
					"$CompilerRoot$\\c2.dll",
					"$CompilerRoot$\\msobj120.dll",
					"$CompilerRoot$\\mspdb120.dll",
					"$CompilerRoot$\\mspdbcore.dll",
					"$CompilerRoot$\\mspft120.dll",
					"$CompilerRoot$\\1033\\clui.dll",
					"$CompilerRoot$\\..\\..\\redist\\x86\\Microsoft.VC120.CRT\\msvcp120.dll",
					"$CompilerRoot$\\..\\..\\redist\\x86\\Microsoft.VC120.CRT\\msvcr120.dll",
					"$CompilerRoot$\\..\\..\\redist\\x86\\Microsoft.VC120.CRT\\vccorlib120.dll"
				};
				extraFiles.insert(extraFiles.end(), &vs2013_extraFiles[0], &vs2013_extraFiles[13]);
			}
			else if (version.compare(0, 3, "17.") != std::string::npos)
			{
				// Using vs2012
				const char *vs2012_extraFiles[12] = {
					"$CompilerRoot$\\c1.dll",
					"$CompilerRoot$\\c1ast.dll",
					"$CompilerRoot$\\c1xx.dll",
					"$CompilerRoot$\\c1xxast.dll",
					"$CompilerRoot$\\c2.dll",
					"$CompilerRoot$\\mspft110.dll",
					"$CompilerRoot$\\1033\\clui.dll",
					"$CompilerRoot$\\..\\..\\redist\\x86\\Microsoft.VC110.CRT\\msvcp110.dll",
					"$CompilerRoot$\\..\\..\\redist\\x86\\Microsoft.VC110.CRT\\msvcr110.dll",
					"$CompilerRoot$\\..\\..\\redist\\x86\\Microsoft.VC110.CRT\\vccorlib110.dll",
					"$CompilerRoot$\\..\\..\\..\\Common7\\IDE\\mspdb110.dll",
					"$CompilerRoot$\\..\\..\\..\\Common7\\IDE\\mspdbcore.dll"
				};
				extraFiles.insert(extraFiles.end(), &vs2012_extraFiles[0], &vs2012_extraFiles[12]);
			}
			else if (version.compare(0, 3, "16.") != std::string::npos)
			{
				// Using vs2010
				const char *vs2010_extraFiles[11] = {
					"$CompilerRoot$\\c1.dll",
					"$CompilerRoot$\\c1xx.dll",
					"$CompilerRoot$\\c2.dll",
					"$CompilerRoot$\\mspft110.dll",
					"$CompilerRoot$\\1033\\clui.dll",
					"$CompilerRoot$\\..\\redist\\x86\\Microsoft.VC100.CRT\\msvcp110.dll",
					"$CompilerRoot$\\..\\redist\\x86\\Microsoft.VC100.CRT\\msvcr110.dll",
					"$CompilerRoot$\\..\\..\\Common7\\IDE\\mspdb100.dll",
					"$CompilerRoot$\\..\\..\\Common7\\IDE\\msobj100.dll",
					"$CompilerRoot$\\..\\..\\Common7\\IDE\\mspdbsrv.exe",
					"$CompilerRoot$\\..\\..\\Common7\\IDE\\mspdbcore.dll"
				};
				extraFiles.insert(extraFiles.end(), &vs2010_extraFiles[0], &vs2010_extraFiles[11]);
			}
		}
	}

private:

};

//----------------------------------------------------------------------------
class cmGlobalFastbuildGenerator::Detail::Definition
{
	struct FastbuildSettings
	{
		std::string cachePath;
	};

	struct FastbuildCompiler
	{
		std::string executable;
	};
	typedef std::map<std::string, FastbuildCompiler> CompilerMap;

	struct FastbuildConfiguration
	{
		std::string name;
	};
	typedef std::map<std::string, FastbuildConfiguration> ConfigurationMap;

	struct FastbuildStructure
	{
		ConfigurationMap configurations;

		FastbuildSettings settings;
		CompilerMap compilers;
	};
};

//----------------------------------------------------------------------------
class cmGlobalFastbuildGenerator::Detail::Generation
{
public:
	struct TargetGenerationContext
	{
		cmGeneratorTarget* target;
		cmLocalFastbuildGenerator* root;
		std::vector<cmLocalGenerator*> generators;
		cmLocalFastbuildGenerator* lg;
	};
	typedef std::map<const cmGeneratorTarget*, TargetGenerationContext> TargetContextMap;
	typedef std::map<std::pair<const cmCustomCommand*,std::string>, std::vector<std::string> > CustomCommandAliasMap;
	typedef Detection::OrderedTargetSet OrderedTargets;

	struct GenerationContext
	{
		GenerationContext(
				cmGlobalFastbuildGenerator * globalGen,
				cmLocalFastbuildGenerator* localGen,
				FileContext& fileCtx)
			: self(globalGen)
			, root(localGen)
			, fc(fileCtx)
		{}
		cmGlobalFastbuildGenerator * self;
		cmLocalFastbuildGenerator* root;
		FileContext& fc;
		OrderedTargets orderedTargets;
		TargetContextMap targetContexts;
		CustomCommandAliasMap customCommandAliases;
	};

	static std::string Quote(const std::string& str, const std::string& quotation = "'")
	{
		std::string result = str;
		cmSystemTools::ReplaceString(result, quotation, "^" + quotation);
		return quotation + result + quotation;
	}

	static std::string Join(const std::vector<std::string>& elems, 
		const std::string& delim)
	{
		std::stringstream stringstream;
		for (std::vector<std::string>::const_iterator iter = elems.begin(); 
			iter != elems.end(); ++iter)
		{
			stringstream << (*iter);
			if (iter + 1 != elems.end()) {
				stringstream << delim;
			}
		}

		return stringstream.str();
	}

	struct WrapHelper
	{
		std::string m_prefix;
		std::string m_suffix;

		std::string operator()(const std::string& in)
		{
			return m_prefix + in + m_suffix;
		}
	};

	static std::vector<std::string> Wrap(const std::vector<std::string>& in, const std::string& prefix = "'", const std::string& suffix = "'")
	{
		std::vector<std::string> result;

		WrapHelper helper = {prefix, suffix};

		std::transform(in.begin(), in.end(),
			std::back_inserter(result), helper);

		return result;
	}

	static std::string EncodeLiteral(const std::string &lit)
	{
		std::string result = lit;
		cmSystemTools::ReplaceString(result, "$", "^$");
		return result;
	}

	static void EnsureDirectoryExists(const std::string& path,
		GenerationContext& context)
	{
		if (cmSystemTools::FileIsFullPath(path.c_str()))
		{
			cmSystemTools::MakeDirectory(path.c_str());
		}
		else
		{
			const std::string fullPath = std::string(
				context.self->GetCMakeInstance()->GetHomeOutputDirectory())
				+ "/" + path;
			cmSystemTools::MakeDirectory(fullPath.c_str());
		}
	}
	
	static void BuildTargetContexts(cmGlobalFastbuildGenerator * gg,
		TargetContextMap& map)
	{
		FBTRACE("BuildTargetContexts\n");
		std::map<std::string, std::vector<cmLocalGenerator*> >::const_iterator
				it = gg->GetProjectMap().begin(),
				end = gg->GetProjectMap().end();
		for(; it != end; ++it)
		{
			FBTRACE("  Project ");
			FBTRACE(it->first.c_str());
			FBTRACE("\n");
			const std::vector<cmLocalGenerator*>& generators = it->second;
			cmLocalFastbuildGenerator* root =
				static_cast<cmLocalFastbuildGenerator*>(generators[0]);

			// Build a map of all targets to their local generator
			for (std::vector<cmLocalGenerator*>::const_iterator iter = generators.begin();
				iter != generators.end(); ++iter)
			{
				cmLocalFastbuildGenerator *lg = static_cast<cmLocalFastbuildGenerator*>(*iter);
				FBTRACE("    LocalGenerator ");
				FBTRACE(lg->GetCurrentSourceDirectory());
				FBTRACE("\n");

				if(gg->IsExcluded(root, lg))
				{
					FBTRACE("    -> EXCLUDED\n");
					continue;
				}

				cmTargets &tgts = lg->GetMakefile()->GetTargets();
				for (cmTargets::iterator targetIter = tgts.begin(); 
					targetIter != tgts.end();
					++targetIter)
				{
					cmTarget &target = (targetIter->second);
					FBTRACE("      Target ");
					FBTRACE(target.GetName().c_str());
					FBTRACE("\n");

					cmGeneratorTarget* gt = lg->FindGeneratorTargetToUse(target.GetName());

					if(gg->IsRootOnlyTarget(gt) &&
						target.GetMakefile() != root->GetMakefile())
					{
						FBTRACE("      -> ROOTONLY\n");
						continue;
					}

					//std::ostringstream s;
					//s << "Creating context for target " << target.GetName() << " in " << lg->GetCurrentSourceDirectory() << " ( 0x" << std::hex << (unsigned long long) gt << std::dec << " ) " << " ( 0x" << std::hex << (unsigned long long) gt->Target << std::dec << " ) " << std::endl;
					//cmSystemTools::Message(s.str().c_str());
					TargetGenerationContext targetContext =
						{ gt, root, generators, lg };
					map[gt] = targetContext;
				}
			}
		}
	}

	static void GenerateRootBFF(cmGlobalFastbuildGenerator * self)
	{

		cmLocalFastbuildGenerator* root = 
			static_cast<cmLocalFastbuildGenerator*>(self->GetLocalGenerators()[0]);

		// Calculate filename
		std::string fname = root->GetMakefile()->GetHomeOutputDirectory();
		fname += "/";
		//fname += root->GetMakefile()->GetProjectName();
		fname += "fbuild";
		fname += ".bff";
		
		// Open file
		cmGeneratedFileStream fout(fname.c_str());
		fout.SetCopyIfDifferent(true);
		if(!fout)
		{
			return;
		}

		FileContext fc(fout);
		GenerationContext context(self, root, fc);
		Detection::ComputeTargetOrderAndDependencies( context.self, context.orderedTargets );
		Detection::StripNestedGlobalTargets( context.orderedTargets );
		BuildTargetContexts( context.self, context.targetContexts );
		WriteRootBFF(context);
		
		// Close file
		if (fout.Close())
		{
			self->FileReplacedDuringGenerate(fname);
		}
	}

	static void WriteRootBFF(GenerationContext& context)
	{
		context.fc.WriteSectionHeader("Fastbuild makefile - Generated using CMAKE");

		FBTRACE("WritePlaceholders\n");
		WritePlaceholders( context );
		FBTRACE("WriteSettings\n");
		WriteSettings( context );
		FBTRACE("WriteCompilers\n");
		WriteCompilers( context );
		FBTRACE("WriteConfigurations\n");
		WriteConfigurations( context );
		FBTRACE("WriteVSConfigurations\n");
		WriteVSConfigurations( context );

		// Sort targets
		std::string buildTargetName;
		std::string finalTargetName;

		FBTRACE("WriteTargetDefinitions\n");
		WriteTargetDefinitions( context, false );
		FBTRACE("WriteAliases\n");
		WriteAliases( context, false, "", buildTargetName);
		FBTRACE("WriteTargetDefinitions (Global)\n");
		WriteTargetDefinitions( context, true );
		FBTRACE("WriteAliases (Global)\n");
		WriteAliases( context, true, buildTargetName, finalTargetName );

		// Write Visual Studio Solution
		FBTRACE("WriteVSSolution\n");
		WriteVSSolution( context, buildTargetName, finalTargetName );
	}

	static void WritePlaceholders(GenerationContext& context)
	{
		// Define some placeholder 
		context.fc.WriteSectionHeader("Helper variables");

		context.fc.WriteVariable( "FB_INPUT_1_PLACEHOLDER", Quote("\"%1\"") );
		context.fc.WriteVariable( "FB_INPUT_2_PLACEHOLDER", Quote("\"%2\"") );
	}

	static void WriteSettings( GenerationContext& context )
	{
		context.fc.WriteSectionHeader("Settings");

		context.fc.WriteCommand("Settings");
		context.fc.WritePushScope();

		//std::string cacheDir =
		//	context.self->GetCMakeInstance()->GetHomeOutputDirectory();
		//cacheDir += "\\.fbuild.cache";
		//cmSystemTools::ConvertToOutputSlashes(cacheDir);

		//context.fc.WriteVariable("CachePath", Quote(cacheDir));
		context.fc.WritePopScope();
	}

	struct CompilerDef
	{
		std::string name;
		std::string path;
		std::string cmakeCompilerID;
		std::string cmakeCompilerVersion;
	};

	static bool WriteCompilers( GenerationContext& context )
	{
		cmMakefile *mf = context.root->GetMakefile();

		context.fc.WriteSectionHeader("Compilers");

		// Detect each language used in the definitions
		std::set<std::string> languages;
		for (TargetContextMap::iterator iter = context.targetContexts.begin();
			iter != context.targetContexts.end(); ++iter)
		{
			TargetGenerationContext& targetContext = iter->second;

			if (targetContext.target->GetType() == cmState::INTERFACE_LIBRARY)
			{
				continue;
			}

			Detection::DetectLanguages(languages, context.self,
				*targetContext.target);
		}

		// Now output a compiler for each of these languages
		typedef std::map<std::string, std::string> StringMap;
		typedef std::map<std::string, CompilerDef> CompilerDefMap;
		CompilerDefMap compilerToDef;
		StringMap languageToCompiler;
		for (std::set<std::string>::iterator iter = languages.begin();
			iter != languages.end();
			++iter)
		{
			const std::string & language = *iter;

			// Calculate the root location of the compiler
			std::string variableString = "CMAKE_"+language+"_COMPILER";
			std::string compilerLocation = mf->GetSafeDefinition(variableString);
			if (compilerLocation.empty())
			{
				return false;
			}

			// Add the language to the compiler's name
			CompilerDef& compilerDef = compilerToDef[compilerLocation];
			if (compilerDef.name.empty())
			{
				compilerDef.name = "Compiler";
				compilerDef.path = compilerLocation;
				compilerDef.cmakeCompilerID = 
					mf->GetSafeDefinition("CMAKE_" + language + "_COMPILER_ID");
				compilerDef.cmakeCompilerVersion =
					mf->GetSafeDefinition("CMAKE_" + language + "_COMPILER_VERSION");
			}
			compilerDef.name += "-";
			compilerDef.name += language;

			// Now add the language to point to that compiler location
			languageToCompiler[language] = compilerLocation;
		}

		// Now output all the compilers
		for (CompilerDefMap::iterator iter = compilerToDef.begin();
			iter != compilerToDef.end();
			++iter)
		{
			const CompilerDef& compilerDef = iter->second;

			// Detect the list of extra files used by this compiler
			// for distribution
			std::vector<std::string> extraFiles;
			Detection::DetectCompilerExtraFiles(compilerDef.cmakeCompilerID,
				compilerDef.cmakeCompilerVersion, extraFiles);

			// Strip out the path to the compiler
			std::string compilerPath = 
				cmSystemTools::GetFilenamePath(compilerDef.path);
			std::string compilerFile = "$CompilerRoot$\\" +
				cmSystemTools::GetFilenameName(compilerDef.path);

			cmSystemTools::ConvertToOutputSlashes(compilerPath);
			cmSystemTools::ConvertToOutputSlashes(compilerFile);

			// Write out the compiler that has been configured
			context.fc.WriteCommand("Compiler", Quote(compilerDef.name));
			context.fc.WritePushScope();
			context.fc.WriteVariable("CompilerRoot", Quote(compilerPath));
			context.fc.WriteVariable("Executable", Quote(compilerFile));
			context.fc.WriteArray("ExtraFiles", Wrap(extraFiles));

			context.fc.WritePopScope();
		}

		// Now output the compiler names according to language as variables
		for (StringMap::iterator iter = languageToCompiler.begin();
			iter != languageToCompiler.end();
			++iter)
		{
			const std::string& language = iter->first;
			const std::string& compilerLocation = iter->second;
			const CompilerDef& compilerDef = compilerToDef[compilerLocation];

			// Output a default compiler to absorb the library requirements for a compiler
			if (iter == languageToCompiler.begin())
			{
				context.fc.WriteVariable("Compiler_dummy", Quote(compilerDef.name));
			}

			context.fc.WriteVariable("Compiler_" + language, Quote(compilerDef.name));
		}
		
		return true;
	}
	
	static void WriteConfigurations(GenerationContext& context)
	{
		context.fc.WriteSectionHeader("Configurations");

		context.fc.WriteVariable("ConfigBase", "");
		context.fc.WritePushScopeStruct();
		context.fc.WritePopScope();

		// Iterate over all configurations and define them:
		std::vector<std::string>::const_iterator
				iter = context.self->GetConfigurations().begin(),
				end = context.self->GetConfigurations().end();
		for (; iter != end; ++iter)
		{
			const std::string & configName = *iter;
			context.fc.WriteVariable("config_" + configName, "");
			context.fc.WritePushScopeStruct();

			// Using base config
			context.fc.WriteCommand("Using", ".ConfigBase");

			context.fc.WritePopScope();
		}

		// Write out a list of all configs
		context.fc.WriteArray("all_configs", 
			Wrap(context.self->GetConfigurations(), ".config_", ""));
	}

	static void WriteCustomCommand(
		GenerationContext& context,
		const cmCustomCommand* cc,
		cmLocalFastbuildGenerator *lg,
		cmGeneratorTarget& target,
		const std::string& configName,
		const std::vector<std::string>& fileOutputs,
		const std::vector<std::string>& symbolicOutputs,
		std::string& targetName,
		const std::string& hostTargetName,
		bool isConfigDependant,
		const std::vector<std::string>& deps)
	{
		cmMakefile* makefile = lg->GetMakefile();

		context.fc.WriteComment("Custom Command " + targetName);
		
		// We need to generate the command for execution.
		cmCustomCommandGenerator ccg(*cc, configName, lg);

		std::string command0 = ccg.GetNumberOfCommands() > 0 ? ccg.GetCommand(0) : std::string();
		Detection::UnescapeFastbuildVariables(command0);
		const cmGeneratorTarget* command0target = ccg.GetNumberOfCommands() > 0 ? ccg.GetCommandTarget(0) : NULL;
		// Take the dependencies listed and split into targets and files.
		std::vector<std::string> depends = ccg.GetDepends();

		for (std::vector<std::string>::iterator iter = depends.begin();
			iter != depends.end();
			++iter)
		{
			std::string& str = *iter;
			Detection::UnescapeFastbuildVariables(str);
			Detection::ResolveFastbuildVariables(str, configName);
		}

		context.fc.WriteComment(std::string("Command0:   ") + command0 + (command0target ? std::string(" ( ") + command0target->GetName() + std::string(" ) ") : std::string()));
		context.fc.WriteCommentMultiLines(std::string("Depends: ") + Join(depends, "\n         "));

		bool hasSymbolicOutput = !symbolicOutputs.empty();

		std::vector<std::string> inputs;
		std::vector<std::string> orderDependencies = deps;

		std::string firstOutputOrSymbolic;
		if (!fileOutputs.empty())
		{
			firstOutputOrSymbolic = fileOutputs.front();
		}
		else if (!symbolicOutputs.empty())
		{
			firstOutputOrSymbolic = symbolicOutputs.front();
			if (isConfigDependant) firstOutputOrSymbolic += "-" + configName;
		}

		// If this exec node always generates outputs,
		// then we need to make sure we don't define outputs multiple times.
		// but if the command should always run (i.e. post builds etc)
		// then we will output a new one.
		if (!firstOutputOrSymbolic.empty())
		{
			// Check if this custom command has already been output.
			// If it has then just drop an alias here to the original
			CustomCommandAliasMap::iterator findResult = context.customCommandAliases.find(std::make_pair(cc, firstOutputOrSymbolic));
			if (findResult != context.customCommandAliases.end())
			{
				const std::vector<std::string>& aliases = findResult->second;
				if (std::find(aliases.begin(), aliases.end(), targetName) != aliases.end())
				{
					context.fc.WriteComment(targetName + ": " + firstOutputOrSymbolic + " already defined");
					// This target has already been generated
					// with the correct name somewhere else.
					return;
				}
				//if (!Detection::isConfigDependant(ccg))
				if (!aliases.empty())
				{
					context.fc.WriteComment(targetName + ": " + firstOutputOrSymbolic + " already defined as " + aliases.front());
					targetName = aliases.front();
					// This command has already been generated.
					// But under a different name so setup an alias to redirect
					// No merged outputs, so this command must always be run.
					// Make it's name unique to its host target
					// This might create a dependency between different configurations (i.e. Debug target being compiled when asking for Release)
					// TODO: We should output a warning about this here...
					//targetName += "-";
					//targetName += hostTargetName;
/*
					std::vector<std::string> targets;
					targets.push_back(*findResult->second.begin());

					context.fc.WriteCommand("Alias", Quote(targetName));
					context.fc.WritePushScope();
					{
						context.fc.WriteArray("Targets",
							Wrap(targets));
					}
					context.fc.WritePopScope();
*/
					return;
				}
			}
			context.customCommandAliases[std::make_pair(cc, firstOutputOrSymbolic)].push_back(targetName);
		}
		else
		{
			// No output, so this command must always be run.
			// Make it's name unique to its host target
			// -> no longer necessary, the targetName is already prefixed by the actual target to eliminate duplicates when the same filename is present in different targets/subdirs
			//targetName += "-";
			//targetName += hostTargetName;
		}
		
		// Take the dependencies listed and split into targets and files.
		for (unsigned int ci = 0; ci < ccg.GetNumberOfCommands(); ++ci)
		{
			const cmGeneratorTarget* cmdtarget = ccg.GetCommandTarget(ci);
			if (cmdtarget != NULL && !cmdtarget->IsImported() && cmdtarget->Target != target.Target)
			{
				orderDependencies.push_back(cmdtarget->GetName() + "-" + configName);
			}
		}
		// Take the dependencies listed and split into targets and files.
		for (std::vector<std::string>::const_iterator iter = depends.begin();
			iter != depends.end(); ++iter)
		{
			const std::string& dep = *iter;

			const cmTarget* depTarget = context.self->FindTarget(dep);
			if (depTarget != NULL)
			{
                if (!depTarget->IsImported() && depTarget != target.Target)
                {
                    orderDependencies.push_back(dep + "-" + configName);
                }
			}
			else
			{
				inputs.push_back(dep);
			}
		}

#ifdef _WIN32
		const std::string shellExt = ".bat";
#else
		const std::string shellExt = ".sh";
#endif

		std::string workingDirectory = ccg.GetWorkingDirectory();
		if (workingDirectory.empty())
		{
			workingDirectory = makefile->GetCurrentBinaryDirectory();
		}
		if (workingDirectory.back() != '/')
		{
			workingDirectory += "/";
		}
		// FIX: workindDirectory may be within the source hierarchy that we are not permitted to write to
		// so the script is now always generated in the current binary directory.
		std::string scriptDirectory = makefile->GetCurrentBinaryDirectory();
		if (scriptDirectory.back() != '/')
		{
			scriptDirectory += "/";
		}

		std::string scriptFileName(scriptDirectory + targetName + shellExt);

		if (ccg.GetNumberOfCommands() > 0)
		{
			cmsys::ofstream scriptFile(scriptFileName.c_str());

			for (unsigned i = 0; i != ccg.GetNumberOfCommands(); ++i)
			{
				std::string args;
				ccg.AppendArguments(i, args);
				cmSystemTools::ReplaceString(args, "$$", "$");
				cmSystemTools::ReplaceString(args, FASTBUILD_DOLLAR_TAG, "$");
#ifdef _WIN32
				//in windows batch, '%' is a special character that needs to be doubled to be escaped
				cmSystemTools::ReplaceString(args, "%", "%%");
#endif
				Detection::ResolveFastbuildVariables(args, configName);

				std::string command(ccg.GetCommand(i));
				cmSystemTools::ReplaceString(command, FASTBUILD_DOLLAR_TAG, "$");
				Detection::ResolveFastbuildVariables(command, configName);

				scriptFile << Quote(command, "\"") << args << std::endl;
			}
		}
		else
		{
			// no command, replace by cd .
			scriptFileName = "cd .";
		}

		// Write out an exec command
		/*
		Exec(alias); (optional)Alias
		{
			.ExecExecutable; Executable to run
			.ExecInput; Input file to pass to executable
			.ExecOutput; Output file generated by executable
			.ExecArguments; (optional)Arguments to pass to executable
			.ExecWorkingDir; (optional)Working dir to set for executable

			; Additional options
			.PreBuildDependencies; (optional)Force targets to be built before this Exec(Rarely needed,
			; but useful when Exec relies on externally generated files).
		}
		*/

		std::for_each(inputs.begin(), inputs.end(), &Detection::UnescapeFastbuildVariables);
		// mergedOutput was already passed through UnescapeFastbuildVariables
		//std::for_each(mergedOutputs.begin(), mergedOutputs.end(), &Detection::UnescapeFastbuildVariables);

		context.fc.WriteCommand("Exec", Quote(targetName));
		context.fc.WritePushScope();
		{
#ifdef _WIN32
			context.fc.WriteVariable("ExecExecutable", Quote(cmSystemTools::FindProgram("cmd.exe")));
			context.fc.WriteVariable("ExecArguments", Quote("/C " + scriptFileName));
#else
			context.fc.WriteVariable("ExecExecutable", Quote(scriptFileName));
#endif
			if(!workingDirectory.empty())
			{
				context.fc.WriteVariable("ExecWorkingDir", Quote(workingDirectory));
			}

			if (inputs.empty())
			{
				//inputs.push_back("dummy-in");
			}
			context.fc.WriteArray("ExecInput", Wrap(inputs));

			std::string output;
			if (!fileOutputs.empty())
			{
				output = fileOutputs.front();
			}
			else
			{
				context.fc.WriteVariable("ExecUseStdOutAsOutput", "true");
				hasSymbolicOutput = true;

				std::string outputDir = target.Target->GetMakefile()->GetCurrentBinaryDirectory();
				output = outputDir + "/dummy-out-" + targetName + ".txt";
			}
			// Currently fastbuild doesn't support more than 1
			// output for a custom command (soon to change hopefully).
			// so only use the first one
			context.fc.WriteVariable("ExecOutput", Quote(output));
			if (hasSymbolicOutput && inputs.empty()) // TODO: probably not the best criteria, works in our use cases...
			{
				context.fc.WriteVariable("ExecAlwaysRun", "true"); // Requires modified version of FASTBuild (after 0.90)
			}
			if (!orderDependencies.empty())
			{
				context.fc.WriteArray("PreBuildDependencies", Wrap(orderDependencies), "+"); // , (configName == "Common") ? "=" : "+");
			}
			
		}
		context.fc.WritePopScope();
	}

	static void WriteCustomBuildSteps(
		GenerationContext& context,
		cmLocalFastbuildGenerator *lg,
		cmGeneratorTarget &target,
		const std::vector<cmCustomCommand>& commands,
		const std::string& buildStep,
		const std::vector<std::string>& orderDeps,
		const std::vector<std::string>& commonDeps = std::vector<std::string>())
	{
		if (commands.empty())
		{
			return;
		}

		const std::string& targetName = target.GetName();
		std::vector<std::string> deps;

		// Now output the commands
		std::vector<std::string>::const_iterator
				iter = context.self->GetConfigurations().begin(),
				end = context.self->GetConfigurations().end();
		for (; iter != end; ++iter)
		{
			const std::string & configName = *iter;

			context.fc.WriteVariable("buildStep_" + buildStep + "_" + configName, "");
			context.fc.WritePushScopeStruct();

			context.fc.WriteCommand("Using", ".BaseConfig_" + configName);
			if (!commonDeps.empty())
			{
				context.fc.WriteArray("PreBuildDependencies",
					Wrap(commonDeps, "'", "-Common'"),
					"+");
			}
			if (!orderDeps.empty())
			{
				context.fc.WriteArray("PreBuildDependencies",
					Wrap(orderDeps, "'", "-" + configName + "'"),
					"+");
			}

			std::string baseName = targetName + "-" + buildStep + "-" + configName;
			int commandCount = 1;
			std::vector<std::string> customCommandTargets;
			for (std::vector<cmCustomCommand>::const_iterator ccIter = commands.begin();
				ccIter != commands.end(); ++ccIter)
			{
				const cmCustomCommand& cc = *ccIter;

				std::stringstream customCommandTargetName;
				customCommandTargetName << baseName << "-" << (commandCount++);

				std::vector<std::string> fileOutputs, symbolicOutputs;
				bool isConfigDependant = false;
				Detection::DetectCustomCommandOutputs(&cc, context.self, lg, target, configName, fileOutputs, symbolicOutputs, isConfigDependant);
				std::string customCommandTargetNameStr = customCommandTargetName.str();
				WriteCustomCommand(context, &cc, lg, target, configName,
					fileOutputs, symbolicOutputs, customCommandTargetNameStr,
					targetName, isConfigDependant, deps);
				customCommandTargets.push_back(customCommandTargetNameStr);
			}

			// Write an alias for this object group to group them all together
			context.fc.WriteCommand("Alias", Quote(baseName));
			context.fc.WritePushScope();
			context.fc.WriteArray("Targets",
				Wrap(customCommandTargets, "'", "'"));
			context.fc.WritePopScope();

			context.fc.WritePopScope();
		}
	}

	static std::pair<bool,bool> WriteCustomBuildRules(
		GenerationContext& context, 
		cmLocalFastbuildGenerator *lg, 
		cmGeneratorTarget &target)
	{
		bool hasCommonCustomCommands = false;
		bool hasConfigCustomCommands = false;
		const std::string& targetName = target.GetName();
		const std::vector<std::string>& configs = context.self->GetConfigurations();

		// Figure out the list of custom build rules in use by this target
		// get a list of source files
		std::vector<cmSourceFile const*> allCustomCommands;
		target.GetCustomCommands(allCustomCommands, "");
		// compute inputs, outputs, and working dir for each config-command combination
		cmMakefile* makefile = lg->GetMakefile();
		std::string currentBinaryDirectory = makefile->GetCurrentBinaryDirectory();

		// Separate config-independant custom commands from specific ones

		std::map< std::string, Detection::DependencySorter::CustomCommandHelper > mapConfigCC;
		std::set<const cmSourceFile*> setConfigDependant;

		for (std::vector<cmSourceFile const*>::const_iterator ccIter = allCustomCommands.begin();
			ccIter != allCustomCommands.end(); ++ccIter)
		{
			const cmSourceFile* sourceFile = *ccIter;
			std::string sourceFilePath = sourceFile->GetFullPath();
			Detection::UnescapeFastbuildVariables(sourceFilePath);

			const cmCustomCommand* cc = sourceFile->GetCustomCommand();
			bool isConfigDependant = (configs.size() <= 1);
			std::ostringstream description;
			description << "CustomCommand " << sourceFilePath << ":\n";
			if (sourceFilePath.find("$ConfigName$") != std::string::npos)
			{
				isConfigDependant = true;
			}
			bool firstConfig = true;
			for (std::vector<std::string>::const_iterator cfIter = configs.begin(); cfIter != configs.end(); ++cfIter, firstConfig = false)
			{
				const std::string& configName = *cfIter;

				// We need to generate the command for execution.
				cmCustomCommandGenerator ccg(*cc, configName, lg);

				std::string& workingDirectory = mapConfigCC[configName].mapWorkingDirectory[sourceFile];
				std::vector<std::string>& inputs = mapConfigCC[configName].mapInputs[sourceFile];
				std::vector<std::string>& fileOutputs = mapConfigCC[configName].mapFileOutputs[sourceFile];
				std::vector<std::string>& symbolicOutputs = mapConfigCC[configName].mapSymbolicOutputs[sourceFile];

				workingDirectory = ccg.GetWorkingDirectory();
				if (workingDirectory.empty())
				{
					workingDirectory = currentBinaryDirectory;
				}
				if (workingDirectory.back() != '/')
				{
					workingDirectory += "/";
				}
				Detection::UnescapeFastbuildVariables(workingDirectory);

				if (firstConfig) description << "    Inputs:\n";

				// Take the dependencies listed and split into targets and files.
				const std::vector<std::string> &depends = ccg.GetDepends();
				for (std::vector<std::string>::const_iterator iter = depends.begin();
					iter != depends.end(); ++iter)
				{
					std::string dep = *iter;
					cmSourceFile* depSourceFile = makefile->GetSource(dep);
					bool isSymbolic = depSourceFile && depSourceFile->GetPropertyAsBool("SYMBOLIC");
					cmTarget* depTarget = context.self->FindTarget(dep);
					if (depSourceFile && !depTarget)
						dep = depSourceFile->GetFullPath();
					Detection::UnescapeFastbuildVariables(dep);
					if (firstConfig) description << "        - " << dep
						<< (depTarget ? std::string(" (Target") + cmState::GetTargetTypeName(depTarget->GetType()) + ")" : std::string())
						<< (depSourceFile ? " (SourceFile)" : "") << (isSymbolic ? " (SYMBOLIC)" : "") << "\n";
					if (depTarget != NULL)
					{
						//inputTargets.push_back(dep);
						// FIX: disable this condition, as it breaks OutDir unit test (using a target executable to generate a header)
						// instead we keep track of this list of target and add it as prebuilddeps
						// isConfigDependant = true;
					}
					else if (depSourceFile != NULL)
					{
						//if (!isSymbolic)
						{
							//dep = depSourceFile->GetFullPath();
							//Detection::UnescapeFastbuildVariables(dep);
							if (dep.find("$ConfigName$") != std::string::npos)
							{
								isConfigDependant = true;
							}
							Detection::ResolveFastbuildVariables(dep, configName);
							inputs.push_back(dep);
						}
						//else
						//{
						//	inputTargets.push_back(dep);
						//}
						// isConfigDependant = true;
					}
					else
					{
						if (!cmSystemTools::FileIsFullPath(dep.c_str()))
						{
							dep = workingDirectory + dep;
						}

						if (dep.find("$ConfigName$") != std::string::npos)
						{
							isConfigDependant = true;
						}
						Detection::ResolveFastbuildVariables(dep, configName);
						inputs.push_back(dep);
					}
				}

				if (firstConfig) description << "    Commands:\n";
				// Add dependencies from command executables
				for (unsigned int ci = 0; ci < ccg.GetNumberOfCommands(); ++ci)
				{
					const cmGeneratorTarget* target = ccg.GetCommandTarget(ci);
					if (target != NULL)
					{
						//inputTargets.push_back(target->GetName() + "-" + configName);
						if (!target->IsImported()) // we ignore imported targets as they are probably not dependant on config (such as Qt4::moc)
						{
							if (firstConfig) description << "        - command \"" << target->GetName() << "\" is a generated target " << cmState::GetTargetTypeName(target->GetType()) << "\n";
							isConfigDependant = true;
						}
					}
					else
					{
						std::string command = ccg.GetCommand(ci);
						Detection::UnescapeFastbuildVariables(command);
						if (command.find("$ConfigName$") != std::string::npos)
						{
							if (firstConfig) description << "        - command \"" << command << "\" uses ConfigName\n";
							isConfigDependant = true;
						}
					}
				}

				// Find outputs and see if they don't depend on the config name, and if we have a real output
				if (firstConfig) description << "    Outputs:\n";
				Detection::DetectCustomCommandOutputs(cc, context.self, lg, target, configName, fileOutputs, symbolicOutputs, isConfigDependant, firstConfig ? &description : NULL);
			}
			if (!isConfigDependant)
			{
				const std::string refConfigName = configs.front();
				const std::string& refWorkingDirectory = mapConfigCC[refConfigName].mapWorkingDirectory[sourceFile];
				const std::vector<std::string>& refInputs = mapConfigCC[refConfigName].mapInputs[sourceFile];
				const std::vector<std::string>& refFileOutputs = mapConfigCC[refConfigName].mapFileOutputs[sourceFile];
				const std::vector<std::string>& refSymbolicOutputs = mapConfigCC[refConfigName].mapSymbolicOutputs[sourceFile];

				for (std::vector<std::string>::const_iterator cfIter = ++configs.begin(); cfIter != configs.end(); ++cfIter)
				{
					const std::string& configName = *cfIter;
					const std::string& workingDirectory = mapConfigCC[configName].mapWorkingDirectory[sourceFile];
					const std::vector<std::string>& inputs = mapConfigCC[configName].mapInputs[sourceFile];
					const std::vector<std::string>& fileOutputs = mapConfigCC[configName].mapFileOutputs[sourceFile];
					const std::vector<std::string>& symbolicOutputs = mapConfigCC[configName].mapSymbolicOutputs[sourceFile];
					if (workingDirectory != refWorkingDirectory || inputs.size() != refInputs.size() || fileOutputs.size() != refFileOutputs.size() || symbolicOutputs.size() != refSymbolicOutputs.size())
					{
						description << " - mismatch in workingDirectory (\"" << workingDirectory << "\"!=\"" << refWorkingDirectory
								  << "\") or number of inputs (" << inputs.size() << "!=" << refInputs.size()
								  << ") or number of outputs (" << fileOutputs.size() << "!=" << refFileOutputs.size() << " or " << symbolicOutputs.size() << "!=" << refSymbolicOutputs.size() << ")\n";
						isConfigDependant = true;
						break;
					}
					for (std::size_t index = 0; index < inputs.size(); ++index)
					{
						if (inputs[index] != refInputs[index])
						{
							description << " - mismatch in an input (\"" << inputs[index] << "\"!=\"" << refInputs[index] << "\")\n";
							isConfigDependant = true;
							break;
						}
					}
					if (isConfigDependant)
					{
						break;
					}
					for (std::size_t index = 0; index < fileOutputs.size(); ++index)
					{
						if (fileOutputs[index] != refFileOutputs[index])
						{
							description << " - mismatch in a file output (\"" << fileOutputs[index] << "\"!=\"" << refFileOutputs[index] << "\")\n";
							isConfigDependant = true;
							break;
						}
					}
					if (isConfigDependant)
					{
						break;
					}
					for (std::size_t index = 0; index < symbolicOutputs.size(); ++index)
					{
						if (symbolicOutputs[index] != refSymbolicOutputs[index])
						{
							description << " - mismatch in a symbolic output (\"" << symbolicOutputs[index] << "\"!=\"" << refSymbolicOutputs[index] << "\")\n";
							isConfigDependant = true;
							break;
						}
					}
					if (isConfigDependant)
					{
						break;
					}
				}
			}
			if (isConfigDependant)
			{
				context.fc.WriteCommentMultiLines("CONFIG " + description.str());
				//configCustomCommands.push_back(sourceFile);
				setConfigDependant.insert(sourceFile);
			}
			else
			{
				context.fc.WriteCommentMultiLines("COMMON " + description.str());
				//commonCustomCommands.push_back(sourceFile);
			}
		}

		if (!allCustomCommands.empty())
		{
			bool firstConfig = true;
			std::string firstConfigName = context.self->GetConfigurations().front();
			const Detection::DependencySorter::CustomCommandHelper& cch0 = mapConfigCC[firstConfigName];
			// Iterating over all configurations
			std::vector<std::string>::const_iterator
				iter = context.self->GetConfigurations().begin(),
				end = context.self->GetConfigurations().end();
			for (; iter != end; ++iter, firstConfig = false)
			{
				std::string configName = *iter;
				Detection::DependencySorter::CustomCommandHelper& cch = mapConfigCC[configName];

				std::vector<const cmSourceFile*>& configCustomCommands = cch.orderedCommands;
				//configCustomCommands = allCustomCommands;
				// copy the commands that could be shared between configs
				for (std::vector<const cmSourceFile*>::const_iterator it = allCustomCommands.begin(); it != allCustomCommands.end(); ++it)
				{
					if (!setConfigDependant.count(*it))
					{
						configCustomCommands.push_back(*it);
					}
				}
				// then the rest of the commands
				for (std::vector<const cmSourceFile*>::const_iterator it = allCustomCommands.begin(); it != allCustomCommands.end(); ++it)
				{
					if (setConfigDependant.count(*it))
					{
						configCustomCommands.push_back(*it);
					}
				}

				// Presort the commands to adjust for dependencies
				// In a number of cases, the commands inputs will be the outputs
				// from another command. Need to sort the commands to output them in order.
				Detection::DependencySorter::Sort(cch, configCustomCommands);

				std::vector<std::string> customCommandTargets;

				unsigned int countCommonCommands = 0;

				//if (firstConfig)
				{
					for (std::vector<const cmSourceFile*>::const_iterator it = configCustomCommands.begin(); it != configCustomCommands.end(); ++it)
					{
						if (!setConfigDependant.count(*it))
						{
							++countCommonCommands;
						}
						else
						{
							break;
						}
					}
				}

				std::string commandNameBase = targetName + "-CustomCommand-Common-";
				std::string commandGroupName = targetName + "-CustomCommands-Common";

				for (unsigned int index = 0; index < configCustomCommands.size(); ++index)
				{
					const cmSourceFile* sourceFile = configCustomCommands[index];
					bool isCommon = index < countCommonCommands;
					std::string activeConfigName = (isCommon ? "Common" : configName);
					if ((firstConfig && index == 0) || index == countCommonCommands) // begin a new block of commands
					{
						customCommandTargets.clear();
						commandNameBase = targetName + "-CustomCommand-" + activeConfigName + "-";
						commandGroupName = targetName + "-CustomCommands-" + activeConfigName;
						context.fc.WriteVariable("CustomCommands_" + activeConfigName, "");
						context.fc.WritePushScopeStruct();
						context.fc.WriteCommand("Using", ".BaseConfig_" + activeConfigName);
					}

					if (!firstConfig && isCommon)
					{
						// no need to output it again, just copy its ref
						cch.mapRuleName[sourceFile] = cch0.mapRuleName.find(sourceFile)->second;
					}
					else
					{

						// write the command
						std::stringstream commandTargetName;
						commandTargetName << commandNameBase << (index+1);
						commandTargetName << "-" << cmSystemTools::GetFilenameName(sourceFile->GetFullPath());

						std::string commandTargetNameStr = commandTargetName.str();
						const std::vector<std::string> & inputs = cch.mapInputs[sourceFile];
						const std::vector<std::string> & fileOutputs = cch.mapFileOutputs[sourceFile];
						const std::vector<std::string> & symbolicOutputs = cch.mapSymbolicOutputs[sourceFile];
						const std::vector<const cmSourceFile*>& inputCmds = cch.mapInputCmds[sourceFile];
						std::vector<std::string> commandDeps;
						for (std::vector<const cmSourceFile*>::const_iterator it = inputCmds.begin(); it != inputCmds.end(); ++it)
						{
							//context.fc.WriteComment("Input Dependency on command " + (*it)->GetFullPath());
							//if (std::find(inputs.begin(), inputs.end(), output0) == inputs.end())
							{
								std::string ruleName = cch.mapRuleName[*it];
								//context.fc.WriteComment("  rule : " + ruleName);
								commandDeps.push_back(ruleName);
							}
						}
						WriteCustomCommand(context, sourceFile->GetCustomCommand(),
							lg, target, configName, fileOutputs, symbolicOutputs, commandTargetNameStr,
							targetName, !isCommon, commandDeps);
						cch.mapRuleName[sourceFile] = commandTargetNameStr;
						customCommandTargets.push_back(commandTargetNameStr);

					}

					if ((index >= countCommonCommands && index == configCustomCommands.size() - 1) || (firstConfig && index == countCommonCommands - 1)) // end of block of commands
					{
						// Write an alias for this object group to group them all together
						context.fc.WriteCommand("Alias", Quote(commandGroupName));
						context.fc.WritePushScope();
						context.fc.WriteArray("Targets",
							Wrap(customCommandTargets, "'", "'"));
						context.fc.WritePopScope();
						context.fc.WritePopScope();
						if (isCommon) hasCommonCustomCommands = true;
						else          hasConfigCustomCommands = true;
					}
				}
			}
		}

		return std::make_pair(hasCommonCustomCommands, hasConfigCustomCommands);
	}

	struct CompileCommand
	{
		std::string defines;
		std::string flags;
		std::map<std::string, std::vector<std::string> > sourceFiles;
	};

	static void WriteTargetDefinition(GenerationContext& context,
		cmLocalFastbuildGenerator *lg, cmGeneratorTarget &target)
	{
		// Detection of the link command as follows:
		std::string linkCommand = "Library";
		switch (target.GetType())
		{
			case cmState::INTERFACE_LIBRARY:
				// We don't write out interface libraries.
				//context.fc.WriteComment(std::string("Target definition: ") + target.GetName() + " (" + cmState::GetTargetTypeName(target.GetType()) + ") IGNORED");
				return;
			case cmState::EXECUTABLE:
			{
				linkCommand = "Executable";
				break;
			}
			case cmState::SHARED_LIBRARY:
			{
				linkCommand = "DLL";
				break;
			}
			case cmState::STATIC_LIBRARY:
			case cmState::MODULE_LIBRARY:
			case cmState::OBJECT_LIBRARY:
			{
				// No changes required
				break;
			}
			case cmState::UTILITY:
			{
				// No link command used 
				linkCommand = "Utility";
				break;
			}
			case cmState::GLOBAL_TARGET:
			{
				// No link command used 
				linkCommand = "Global";
				break;
			}
			case cmState::UNKNOWN_LIBRARY:
			{
				// Ignoring this target generation...
				//context.fc.WriteComment(std::string("Target definition: ") + target.GetName() + " (" + cmState::GetTargetTypeName(target.GetType()) + ") IGNORED");
				return;
			}
		}

		const std::string& targetName = target.GetName();

		// Object libraries do not have linker stages
		// nor utilities
		bool hasLinkerStage =
			target.GetType() != cmState::OBJECT_LIBRARY &&
			target.GetType() != cmState::UTILITY &&
			target.GetType() != cmState::GLOBAL_TARGET;

		bool hasOutput =
			target.GetType() != cmState::UTILITY &&
			target.GetType() != cmState::GLOBAL_TARGET;

		context.fc.WriteComment(std::string("Target definition: ")+targetName+" (" + linkCommand + ")");
		context.fc.WritePushScope();

		std::vector<std::string> dependencies;
		std::vector<std::string> linkDependencies;
		Detection::DetectTargetCompileDependencies(context.self, target, dependencies, hasLinkerStage?linkDependencies:dependencies);
		if (!dependencies.empty())
			context.fc.WriteComment(std::string("Target dependencies: ") + Join(dependencies," "));
		if (!linkDependencies.empty())
			context.fc.WriteComment(std::string("Target link dependencies: ") + Join(linkDependencies, " "));

		// Output common config (for custom commands that do not depend on current config
		if (context.self->GetConfigurations().size() > 1)
		{
			const std::string configName = "Common";

			context.fc.WriteVariable("BaseConfig_" + configName, "");
			context.fc.WritePushScopeStruct();
			context.fc.WriteCommand("Using", ".ConfigBase");
			//context.fc.WriteVariable("ConfigName", Quote(configName));

			// Write the dependency list in here too
			// So all dependant targets are built before this one is
			// This is incase this target depends on code generated from previous ones
			// CHANGE: now we distinguish dependancies for link, so that compile can be in parallel.
			// Dependencies on generated codes should be explicit based on the files themselves
			{
				context.fc.WriteArray("PreBuildDependencies",
					Wrap(dependencies, "'", "-" + configName + "'"));
			}

			context.fc.WritePopScope();
		}

		// Iterate over each configuration
		// This time to define linker settings for each config
		std::vector<std::string>::const_iterator
				configIter = context.self->GetConfigurations().begin(),
				configEnd = context.self->GetConfigurations().end();
		for (; configIter != configEnd; ++configIter)
		{
			const std::string & configName = *configIter;

			context.fc.WriteVariable("BaseConfig_" + configName, "");
			context.fc.WritePushScopeStruct();

			context.fc.WriteCommand("Using", ".ConfigBase");

			context.fc.WriteVariable("ConfigName", Quote(configName));

			// Write out the output paths for the outcome of this target
			if (hasOutput)
			{
				context.fc.WriteBlankLine();
				context.fc.WriteComment("General output details:");

				Detection::FastbuildTargetNames targetNames;
				Detection::DetectOutput(targetNames, target, configName);

				context.fc.WriteVariable("TargetNameOut", Quote(targetNames.targetNameOut));
				context.fc.WriteVariable("TargetNameImport", Quote(targetNames.targetNameImport));
				context.fc.WriteVariable("TargetNamePDB", Quote(targetNames.targetNamePDB));
				context.fc.WriteVariable("TargetNameSO", Quote(targetNames.targetNameSO));
				context.fc.WriteVariable("TargetNameReal", Quote(targetNames.targetNameReal));

				// TODO: Remove this if these variables aren't used... 
				// They've been added for testing
				context.fc.WriteVariable("TargetOutput", Quote(targetNames.targetOutput));
				context.fc.WriteVariable("TargetOutputImplib", Quote(targetNames.targetOutputImplib));
				context.fc.WriteVariable("TargetOutputReal", Quote(targetNames.targetOutputReal));
				context.fc.WriteVariable("TargetOutDir", Quote(targetNames.targetOutputDir));
				context.fc.WriteVariable("TargetOutCompilePDBDir", Quote(targetNames.targetOutputCompilePDBDir));
				context.fc.WriteVariable("TargetOutPDBDir", Quote(targetNames.targetOutputPDBDir));

				// Compile directory always needs to exist
				EnsureDirectoryExists(targetNames.targetOutputCompilePDBDir, context);

				if (target.GetType() != cmState::OBJECT_LIBRARY)
				{
					// on Windows the output dir is already needed at compile time
					// ensure the directory exists (OutDir test)
					EnsureDirectoryExists(targetNames.targetOutputDir, context);
					EnsureDirectoryExists(targetNames.targetOutputPDBDir, context);
				}
			}

			// Write the dependency list in here too
			// So all dependant libraries are built before this one is
			// This is incase this library depends on code generated from previous ones
			// CHANGE: now we distinguish dependancies for link, so that compile can be in parallel.
			// Dependencies on generated codes should be explicit based on the files themselves
			{
				context.fc.WriteArray("PreBuildDependencies",
					Wrap(dependencies, "'", "-" + configName + "'"));
			}

			context.fc.WritePopScope();
		}

		// Output the prebuild/Prelink commands
		WriteCustomBuildSteps(context, lg, target, target.GetPreBuildCommands(), "PreBuild", dependencies);
		WriteCustomBuildSteps(context, lg, target, target.GetPreLinkCommands(), "PreLink", dependencies);

		// Output the ExportAll (Prelink) command if WINDOWS_EXPORT_ALL_SYMBOLS is on
		bool addExportAll = false;
		std::string exportAllOutDir;
		std::string exportAllDefFile;
		std::string exportAllObjsFile;
		std::map<std::string, std::vector<std::string> > exportAllConfigObjectFiles;

		if (target.GetType() == cmState::SHARED_LIBRARY &&
			target.Target->GetMakefile()->IsOn("CMAKE_SUPPORT_WINDOWS_EXPORT_ALL_SYMBOLS") &&
			target.GetPropertyAsBool("WINDOWS_EXPORT_ALL_SYMBOLS"))
		{
			addExportAll = true;

			std::vector<cmCustomCommand> commands;
			std::vector<std::string> outputs;
			exportAllOutDir = target.ObjectDirectory;
			exportAllDefFile = "exportall.def";
			exportAllObjsFile = "objects.txt";
		}

		// Write the custom build rules
		std::pair<bool, bool> hasCustomBuildRules = WriteCustomBuildRules(context, lg, target);

		std::vector<std::string> preBuildSteps;
		if (hasCustomBuildRules.second)
		{
			preBuildSteps.push_back("CustomCommands");
		}
		if (!target.GetPreBuildCommands().empty())
		{
			preBuildSteps.push_back("PreBuild");
		}
		if (!target.GetPreLinkCommands().empty())
		{
			preBuildSteps.push_back("PreLink");
		}

		// Figure out the list of languages in use by this target
		std::set<std::string> languages;
		Detection::DetectLanguages(languages, context.self, target);

		if (!languages.empty())
		{
			// Iterate over each configuration
			// This time to define prebuild and post build targets for each config
			configIter = context.self->GetConfigurations().begin();
			configEnd = context.self->GetConfigurations().end();
			for (; configIter != configEnd; ++configIter)
			{
				const std::string & configName = *configIter;

				context.fc.WriteVariable("BaseCompilationConfig_" + configName, "");
				context.fc.WritePushScopeStruct();

				context.fc.WriteCommand("Using", ".BaseConfig_" + configName);

				// Add to the list of prebuild deps
				// The prelink and prebuild commands
				if (!preBuildSteps.empty() || hasCustomBuildRules.first)
				{
					std::vector<std::string> deps = Wrap(preBuildSteps, "'" + targetName + "-", "-" + configName + "'");
					if (hasCustomBuildRules.first)
						deps.push_back(std::string("'") + targetName + "-CustomCommands-Common'");
					context.fc.WriteArray("PreBuildDependencies",
						deps,
						"+");
				}

				context.fc.WritePopScope();
			}

		}
		
		std::vector<std::string> linkableDeps;
		std::vector<std::string> orderDeps;
		std::vector<std::string> commonDeps;

		// Write the object list definitions for each language
		// stored in this target
		for (std::set<std::string>::iterator langIter = languages.begin();
			langIter != languages.end(); ++langIter)
		{
			const std::string & objectGroupLanguage = *langIter;
			std::string ruleObjectGroupName = "ObjectGroup_" + objectGroupLanguage;
			linkableDeps.push_back(ruleObjectGroupName);

			context.fc.WriteVariable(ruleObjectGroupName, "");
			context.fc.WritePushScopeStruct();

			// Iterating over all configurations
			configIter = context.self->GetConfigurations().begin();
			for (; configIter != configEnd; ++configIter)
			{
				const std::string & configName = *configIter;
				std::vector<std::string>& exportAllObjectFiles = exportAllConfigObjectFiles[configName];
				context.fc.WriteVariable("ObjectConfig_" + configName, "");
				context.fc.WritePushScopeStruct();

				context.fc.WriteCommand("Using", ".BaseCompilationConfig_" + configName);

				context.fc.WriteBlankLine();
				context.fc.WriteComment("Compiler options:");

				// Compiler options
				std::string baseCompileFlags;
				{
					// Remove the command from the front and leave the flags behind
					std::string compileCmd;
					Detection::DetectBaseCompileCommand(compileCmd,
						lg, target, objectGroupLanguage, configName);

					// No need to double unescape the variables
					//Detection::UnescapeFastbuildVariables(compileCmd);

					std::string executable;
					Detection::SplitExecutableAndFlags(compileCmd, executable, baseCompileFlags);

					context.fc.WriteVariable("CompilerCmdBaseFlags", Quote(baseCompileFlags));

					std::string compilerName = ".Compiler_" + objectGroupLanguage;
					context.fc.WriteVariable("Compiler", compilerName);
				}

				std::map<std::string,CompileCommand> commandPermutations;

				// Source files
				context.fc.WriteBlankLine();
				context.fc.WriteComment("Source files:");
				{
					// get a list of source files
					std::vector<cmSourceFile const*> objectSources;
					target.GetObjectSources(objectSources, configName);

					std::vector<cmSourceFile const*> filteredObjectSources;
					Detection::FilterSourceFiles(filteredObjectSources, objectSources,
						objectGroupLanguage);

					// Figure out the compilation commands for all
					// the translation units in the compilation.
					// Detect if one of them is a PreCompiledHeader
					// and extract it to be used in a precompiled header
					// generation step.
					for (std::vector<cmSourceFile const*>::iterator sourceIter = filteredObjectSources.begin();
						sourceIter != filteredObjectSources.end(); ++sourceIter)
					{
						cmSourceFile const *srcFile = *sourceIter;
						std::string sourceFile = srcFile->GetFullPath();
						Detection::UnescapeFastbuildVariables(sourceFile);
						Detection::ResolveFastbuildVariables(sourceFile, configName);
						std::string sourceDir = srcFile->GetLocation().GetDirectory();
						Detection::UnescapeFastbuildVariables(sourceDir);
						Detection::ResolveFastbuildVariables(sourceDir, configName);

						// Detect flags and defines
						std::string compilerFlags;
						Detection::DetectCompilerFlags(compilerFlags, 
							lg, target, srcFile, objectGroupLanguage, configName);
						std::string compileDefines = 
							Detection::ComputeDefines(lg, target, srcFile, configName, objectGroupLanguage);
						
						Detection::UnescapeFastbuildVariables(compilerFlags);
						Detection::UnescapeFastbuildVariables(compileDefines);

						std::string configKey = compilerFlags + "{|}" + compileDefines;
						CompileCommand& command = commandPermutations[configKey];
						command.sourceFiles[sourceDir].push_back(sourceFile);
						command.flags = compilerFlags;
						command.defines = compileDefines;
					}
				}

				// Iterate over all subObjectGroups
				std::string objectGroupRuleName = targetName + "-" + ruleObjectGroupName + "-" + configName;
				std::vector<std::string> configObjectGroups;
				int groupNameCount = 1;
				for (std::map<std::string, CompileCommand>::iterator groupIter = commandPermutations.begin();
					groupIter != commandPermutations.end();
					++groupIter)
				{
					context.fc.WritePushScope();

					const CompileCommand& command = groupIter->second;

					context.fc.WriteVariable("CompileDefineFlags", Quote( command.defines ));
					context.fc.WriteVariable("CompileFlags", Quote( command.flags ));
					context.fc.WriteVariable("CompilerOptions", Quote("$CompileFlags$ $CompileDefineFlags$ $CompilerCmdBaseFlags$"));
					std::string outputExtension;
					if(objectGroupLanguage == "RC")
					{
						outputExtension = ".res";
					}
					else
					{
						outputExtension = "." + objectGroupLanguage + ".obj";
					}
					context.fc.WriteVariable("CompilerOutputExtension", Quote(outputExtension));

					std::map<std::string, std::vector<std::string> >::const_iterator objectListIt;
					for(objectListIt = command.sourceFiles.begin(); objectListIt != command.sourceFiles.end(); ++objectListIt)
					{
						const std::string folderName(Detection::GetLastFolderName(objectListIt->first));
						std::stringstream ruleName;
						ruleName << objectGroupRuleName << "-" << folderName << "-" << (groupNameCount++);

						context.fc.WriteCommand("ObjectList", Quote(ruleName.str()));
						context.fc.WritePushScope();

						context.fc.WriteArray("CompilerInputFiles",
							Wrap(objectListIt->second, "'", "'"));

						configObjectGroups.push_back(ruleName.str());

						std::string targetCompileOutDirectory =
							Detection::DetectTargetCompileOutputDir(lg, target, configName);
						std::string outputPath = targetCompileOutDirectory + folderName;
						context.fc.WriteVariable("CompilerOutputPath", Quote(outputPath));

						// Unity source files:
						context.fc.WriteVariable("UnityInputFiles", ".CompilerInputFiles");

						/*
						if (Detection::DetectPrecompiledHeader(command.flags + " " +
							baseCompileFlags + " " + command.defines,
							preCompiledHeaderInput,
							preCompiledHeaderOutput,
							preCompiledHeaderOptions)
						*/

						if (addExportAll && objectGroupLanguage != "RC")
						{
							for (std::vector<std::string>::const_iterator it = objectListIt->second.begin(); it != objectListIt->second.end(); ++it)
							{
								const std::string filename = cmSystemTools::GetFilenameWithoutExtension(*it);
								std::string objfile = outputPath + '/' + filename + outputExtension;
								exportAllObjectFiles.push_back(objfile);
							}
						}

						context.fc.WritePopScope();
					}

					context.fc.WritePopScope();
				}

				if(!configObjectGroups.empty()) {
					// Write an alias for this object group to group them all together
					context.fc.WriteCommand("Alias", Quote(objectGroupRuleName));
					context.fc.WritePushScope();
					context.fc.WriteArray("Targets",
						Wrap(configObjectGroups, "'", "'"));
					context.fc.WritePopScope();
				}

				context.fc.WritePopScope();
			}
			context.fc.WritePopScope();
		}
		
		if (addExportAll)
		{

			// Iterating over all configurations
			configIter = context.self->GetConfigurations().begin();
			for (; configIter != configEnd; ++configIter)
			{
				const std::string & configName = *configIter;
				const std::vector<std::string>& exportAllObjectFiles = exportAllConfigObjectFiles[configName];
				std::string obj_dir_expanded = exportAllOutDir;
				Detection::UnescapeFastbuildVariables(obj_dir_expanded);
				Detection::ResolveFastbuildVariables(obj_dir_expanded, configName);
				cmSystemTools::MakeDirectory(obj_dir_expanded.c_str());
				std::string objs_file = obj_dir_expanded + exportAllObjsFile;
				cmGeneratedFileStream fout(objs_file.c_str());
				if (!fout) {
					cmSystemTools::Error("could not open ", objs_file.c_str());
					continue;
				}
				for (std::vector<std::string>::const_iterator it = exportAllObjectFiles.begin(); it != exportAllObjectFiles.end(); ++it)
				{
					fout << *it << "\n";
				}
			}

			std::vector<std::string> outputs;
			outputs.push_back(exportAllOutDir + exportAllDefFile);

			std::vector<std::string> empty;

			std::string cmakeCommand = cmSystemTools::GetCMakeCommand();
			cmSystemTools::ConvertToWindowsExtendedPath(cmakeCommand);
			cmCustomCommandLine cmdl;
			cmdl.push_back(cmakeCommand);
			cmdl.push_back("-E");
			cmdl.push_back("__create_def");
			cmdl.push_back(exportAllOutDir + exportAllDefFile);
			cmdl.push_back(exportAllOutDir + exportAllObjsFile);
			cmCustomCommandLines commandLines;
			commandLines.push_back(cmdl);
			std::vector<cmCustomCommand> commands;
			cmCustomCommand command(target.Target->GetMakefile(), outputs, empty, empty,
				commandLines, "Auto build dll exports", ".");
			commands.push_back(command);

			//context.fc.WriteArray("Libraries",
			//	Wrap(linkableDeps, "'" + targetName + "-", "-" + configName + "'"));

			WriteCustomBuildSteps(context, lg, target, commands, "ExportAll", linkableDeps);

		}

		// Iterate over each configuration
		// This time to define linker settings for each config
		std::vector<std::string>::const_iterator
				iter = context.self->GetConfigurations().begin(),
				end = context.self->GetConfigurations().end();
		for (; iter != end; ++iter)
		{
			const std::string & configName = *iter;

			std::string linkRuleName = targetName + "-link-" + configName;

			if (hasLinkerStage)
			{

				context.fc.WriteVariable("LinkerConfig_" + configName, "");
				context.fc.WritePushScopeStruct();

				context.fc.WriteCommand("Using", ".BaseConfig_" + configName);
				if (!linkDependencies.empty())
				{
					context.fc.WriteArray("PreBuildDependencies",
						Wrap(linkDependencies, "'", "-" + configName + "'"), "+");
				}

				context.fc.WriteBlankLine();
				context.fc.WriteComment("Linker options:");
				// Linker options
				{
					std::string linkLibs;
					std::string targetFlags;
					std::string linkFlags;
					std::string frameworkPath;
					std::string dummyLinkPath;

					lg->GetTargetFlags(
						linkLibs,
						targetFlags,
						linkFlags,
						frameworkPath,
						dummyLinkPath,
						&target,
						false,
						configName);
					if (!dummyLinkPath.empty())
					{
						context.fc.WriteComment("Link Path: " + dummyLinkPath);
					}

					std::string linkPath;
					Detection::DetectLinkerLibPaths(linkPath, lg, target, configName);

					Detection::UnescapeFastbuildVariables(linkLibs);
					Detection::UnescapeFastbuildVariables(targetFlags);
					Detection::UnescapeFastbuildVariables(linkFlags);
					Detection::UnescapeFastbuildVariables(frameworkPath);
					Detection::UnescapeFastbuildVariables(linkPath);

					linkPath = frameworkPath + linkPath;

					if(addExportAll || target.IsExecutableWithExports())
					{
						const char* defFileFlag = target.Makefile->GetDefinition("CMAKE_LINK_DEF_FILE_FLAG");
						std::string defFilePath;
						if (addExportAll)
						{
							defFilePath = exportAllOutDir;
							Detection::UnescapeFastbuildVariables(defFilePath);
							Detection::ResolveFastbuildVariables(defFilePath, configName);
							defFilePath += exportAllDefFile;
						}
						else
						{
							const cmSourceFile* defFile = target.GetModuleDefinitionFile(configName);
							if (defFile)
							{
								defFilePath = defFile->GetFullPath();
							}
						}
						if (defFileFlag && !defFilePath.empty())
						{
							if (!linkFlags.empty())
							{
								linkFlags += ' ';
							}
							linkFlags += defFileFlag + defFilePath;
						}
					}

					context.fc.WriteVariable("LinkPath", "'" + linkPath + "'");
					context.fc.WriteVariable("LinkLibs", "'" + linkLibs + "'");
					context.fc.WriteVariable("LinkFlags", "'" + linkFlags + "'");
					context.fc.WriteVariable("TargetFlags", "'" + targetFlags + "'");

					// Remove the command from the front and leave the flags behind
					std::string linkCmd;
					if (!Detection::DetectBaseLinkerCommand(linkCmd,
						lg, target, configName))
					{
						return;
					}
					// No need to do this, because the function above has already escaped things appropriately
					//Detection::UnescapeFastbuildVariables(linkCmd);

					std::string executable;
					std::string flags;
					Detection::SplitExecutableAndFlags(linkCmd, executable, flags);

					std::string language = target.GetLinkerLanguage(configName);
					std::string linkerType = lg->GetMakefile()->GetSafeDefinition("CMAKE_" + language + "_COMPILER_ID");

					context.fc.WriteVariable("Linker", Quote(executable));
					
					context.fc.WriteVariable("LinkerType", Quote(linkerType));

					std::string libExecutable = "$Linker$";
					// we must use the librarian (lib.exe) as link.exe won't accept /lib in a response file.
					// Fastbuild chooses to write a response file if the arguments are too long:
					if (linkerType == "MSVC" && linkCommand == "Library")
					{
						std::string flagsLower = flags;
						std::transform(flagsLower.begin(), flagsLower.end(), flagsLower.begin(), ::tolower);
						size_t found = flagsLower.find("/lib ");
						if (found != std::string::npos)
							flags = flags.replace(found, strlen("/lib "), "");

						std::string exeLower = executable;
						std::transform(exeLower.begin(), exeLower.end(), exeLower.begin(), ::tolower);
						found = exeLower.find("link.exe");
						if (found != std::string::npos)
							libExecutable = executable.replace(found, strlen("link.exe"), "lib.exe");
					}
					
					context.fc.WriteVariable("BaseLinkerOptions", Quote(flags));

					context.fc.WriteVariable("LinkerOutput", "'$TargetOutput$'");
					context.fc.WriteVariable("LinkerOptions", "'$BaseLinkerOptions$ $LinkLibs$'");

					context.fc.WriteArray("Libraries", 
						Wrap(linkableDeps, "'" + targetName + "-", "-" + configName + "'"));

					// Now detect the extra dependencies for linking
					{
						std::vector<std::string> extraDependencies;
						Detection::DetectTargetObjectDependencies( context.self, target, configName, extraDependencies );
						//Detection::DetectTargetLinkDependencies(target, configName, extraDependencies);
						std::ostringstream log;
						Detection::DetectTargetLinkItems(target, configName, extraDependencies, log);
						context.fc.WriteCommentMultiLines(log.str().c_str());

						std::for_each(extraDependencies.begin(), extraDependencies.end(), Detection::UnescapeFastbuildVariables);

						context.fc.WriteArray("Libraries", 
							Wrap(extraDependencies, "'", "'"),
							"+");
						std::vector<std::string> otherInputs;
						Detection::DetectTargetLinkOtherInputs(lg, target, configName, otherInputs);
						std::for_each(otherInputs.begin(), otherInputs.end(), Detection::UnescapeFastbuildVariables);
						if (!otherInputs.empty())
						{
							context.fc.WriteArray("LinkerOtherInputs",
								Wrap(otherInputs, "'", "'"));
						}
					}
				
					context.fc.WriteCommand(linkCommand, Quote(linkRuleName));
					context.fc.WritePushScope();
					if (linkCommand == "Library")
					{
						context.fc.WriteComment("Convert the linker options to work with libraries");

						// Push dummy definitions for compilation variables
						// These variables are required by the Library command
						context.fc.WriteVariable("Compiler", ".Compiler_dummy");
						context.fc.WriteVariable("CompilerOptions", "'-c $FB_INPUT_1_PLACEHOLDER$ $FB_INPUT_2_PLACEHOLDER$'");
						context.fc.WriteVariable("CompilerOutputPath", "'/dummy/'");
					
						// These variables are required by the Library command as well
						// we just need to transfer the values in the linker variables
						// to these locations
						context.fc.WriteVariable("Librarian",Quote(libExecutable));
						context.fc.WriteVariable("LibrarianOptions","'$LinkerOptions$'");
						context.fc.WriteVariable("LibrarianOutput","'$LinkerOutput$'");

						context.fc.WriteVariable("LibrarianAdditionalInputs", ".Libraries");
					}
					if (addExportAll)
					{
						std::vector<std::string> deps;
						deps.push_back(std::string("'") + targetName + "-ExportAll-" + configName + "'");
						context.fc.WriteArray("PreBuildDependencies",
							deps,
							"+");
					}
					context.fc.WritePopScope();
				}
				context.fc.WritePopScope();
			}
		}

		if (!target.GetPreBuildCommands().empty())
		{
			orderDeps.push_back("PreBuild");
		}
		if (!target.GetPreLinkCommands().empty())
		{
			orderDeps.push_back("PreLink");
		}
		if (hasCustomBuildRules.first)
		{
			commonDeps.push_back("CustomCommands");
		}
		if (hasCustomBuildRules.second)
		{
			orderDeps.push_back("CustomCommands");
		}
		if (addExportAll)
		{
			orderDeps.push_back("ExportAll");
		}
		if (hasLinkerStage)
		{
			linkableDeps.push_back("link");
			orderDeps.push_back("link");
		}

		// Output the postbuild commands
		WriteCustomBuildSteps(context, lg, target, target.GetPostBuildCommands(), "PostBuild", 
			Wrap(orderDeps, targetName + "-", ""), Wrap(commonDeps, targetName + "-", ""));

		// Always add the pre/post build steps as
		// part of the alias.
		// This way, if there are ONLY build steps, then
		// things should still work too.
		if (!target.GetPostBuildCommands().empty())
		{
			orderDeps.push_back("PostBuild");
		}

		// Output a list of aliases
		WriteTargetAliases(context, target, linkableDeps, orderDeps, commonDeps, dependencies);

		// Output Visual Studio Project info
		WriteVSProject(context, target);

		context.fc.WritePopScope();
	}

	static void WriteTargetAliases(
		GenerationContext& context,
		cmGeneratorTarget& target,
		const std::vector<std::string>& linkableDeps,
		const std::vector<std::string>& orderDeps,
		const std::vector<std::string>& commonDeps,
		const std::vector<std::string>& targetDeps)
	{
		const std::string& targetName = target.GetName();

		if (context.self->GetConfigurations().size() > 1)
		{
			{
				context.fc.WriteCommand("Alias",
					Quote(targetName + "-Common"));
				context.fc.WritePushScope();
				if (!commonDeps.empty())
				{
					context.fc.WriteArray("Targets",
						Wrap(commonDeps, "'" + targetName + "-", "-Common'"));
				}
				else
				{
					context.fc.WriteArray("Targets",
						Wrap(targetDeps, "'", "-Common'"));
				}
				context.fc.WritePopScope();
			}
		}
		
		std::vector<std::string>::const_iterator
			iter = context.self->GetConfigurations().begin(),
			end = context.self->GetConfigurations().end();
		for (; iter != end; ++iter)
		{
			const std::string & configName = *iter;
			if (!linkableDeps.empty())
			{
				context.fc.WriteCommand("Alias",
					Quote(targetName + "-" + configName + "-products"));
				context.fc.WritePushScope();
				context.fc.WriteArray("Targets",
					Wrap(linkableDeps, "'" + targetName + "-", "-" + configName + "'"));
				context.fc.WritePopScope();
			}

			// Always output targetName-Config alias for all configs, even when they did not output anything
			//if (!orderDeps.empty() || !commonDeps.empty() || !linkableDeps.empty())
			{
				context.fc.WriteCommand("Alias",
					Quote(targetName + "-" + configName));
				context.fc.WritePushScope();
				std::string targetsOp = "=";
				if (!linkableDeps.empty())
				{
					context.fc.WriteArray("Targets",
						Wrap(linkableDeps, "'" + targetName + "-", "-" + configName + "'"),
						targetsOp);
					targetsOp = "+";
				}
				if (!commonDeps.empty())
				{
					context.fc.WriteArray("Targets",
						Wrap(commonDeps, "'" + targetName + "-", "-Common'"),
						targetsOp);
					targetsOp = "+";
				}
				if (!orderDeps.empty())
				{
					context.fc.WriteArray("Targets",
						Wrap(orderDeps, "'" + targetName + "-", "-" + configName + "'"),
						targetsOp);
					targetsOp = "+";
				}
				if (targetsOp == "=" && !targetDeps.empty())
				{
					// no active output, directly refer to the cmake target dependencies instead
					context.fc.WriteArray("Targets",
						Wrap(targetDeps, "'", "-" + configName + "'"),
						targetsOp);
					targetsOp = "+";
				}
				if (targetsOp == "=")
				{ // this target really is empty, but we still have to write Targets
					std::vector<std::string> empty;
					context.fc.WriteArray("Targets",empty,
						targetsOp);
				}
				context.fc.WritePopScope();
			}
		}
	}

	static void WriteTargetUtilityDefinition(GenerationContext& context,
		cmLocalFastbuildGenerator *lg, cmGeneratorTarget& target)
	{
		WriteTargetDefinition(context, lg, target);
	}

	static void WriteTargetDefinitions(GenerationContext& context,
		bool outputGlobals)
	{
		context.fc.WriteSectionHeader((outputGlobals)?"Global Target Definitions":"Target Definitions");

		// Now iterate each target in order
		for (OrderedTargets::iterator targetIter = context.orderedTargets.begin(); 
			targetIter != context.orderedTargets.end();
			++targetIter)
		{
			const cmGeneratorTarget* constTarget = (*targetIter);
			//context.fc.WriteComment(std::string("Processing Target: ") + constTarget->GetName() + " (" + cmState::GetTargetTypeName(constTarget->GetType()) + ")");

			if (constTarget->GetType() == cmState::GLOBAL_TARGET)
			{
				if (!outputGlobals)
					continue;
			}
			else
			{
				if (outputGlobals)
					continue;
			}
			FBTRACE("  Target ");
			FBTRACE(constTarget->GetName().c_str());
			FBTRACE("\n");
			if(constTarget->GetType() == cmState::INTERFACE_LIBRARY)
			{
				FBTRACE("    Skipping INTERFACE_LIBRARY target\n");
				continue;
			}

			TargetContextMap::iterator findResult = context.targetContexts.find(constTarget);
			if (findResult == context.targetContexts.end())
			{
				std::ostringstream s;
				s << "No TargetContext found for target " << constTarget->GetName() << " in " << constTarget->LocalGenerator->GetCurrentSourceDirectory() << " ( 0x" << std::hex << (unsigned long long) constTarget << std::dec << " ) " << " ( 0x" << std::hex << (unsigned long long) constTarget->Target << std::dec << " ) " << std::endl;
				cmSystemTools::Error(s.str().c_str());
				cmSystemTools::SetFatalErrorOccured();
				continue;
			}

			cmGeneratorTarget* target = findResult->second.target;
			cmLocalFastbuildGenerator* lg = findResult->second.lg;

			switch (target->GetType())
			{
				case cmState::EXECUTABLE:
				case cmState::SHARED_LIBRARY:
				case cmState::STATIC_LIBRARY:
				case cmState::MODULE_LIBRARY:
				case cmState::OBJECT_LIBRARY:
					FBTRACE("    WriteTargetDefinition\n");
					WriteTargetDefinition(context, lg, *target);
					break;
				case cmState::UTILITY:
				case cmState::GLOBAL_TARGET:
					FBTRACE("    WriteTargetUtilityDefinition\n");
					WriteTargetUtilityDefinition(context, lg, *target);
					break;
				default:
					break;
			}
		}
	}

	static bool IsPartOfDefaultBuild(GenerationContext& context, cmGeneratorTarget* target, const std::string& configName)
	{
		if (!context.self->IsExcluded(context.root, target))
		{
			return true;
		}
		if (target->GetType() == cmState::GLOBAL_TARGET)
		{
			// check if INSTALL or PACKAGE target is part of default build
			// by inspect CMAKE_VS_INCLUDE_<Target>_TO_DEFAULT_BUILD properties
			// (i.e. CMAKE_VS_INCLUDE_INSTALL_TO_DEFAULT_BUILD for INSTALL)
			if (target->GetName() == "INSTALL" ||
				target->GetName() == "PACKAGE")
			{
				std::string definitionName = "CMAKE_VS_INCLUDE_" + target->GetName() + "_TO_DEFAULT_BUILD";
				const char* propertyValue = target->Target->GetMakefile()->GetDefinition(definitionName);
				cmGeneratorExpression ge;
				cmsys::auto_ptr<cmCompiledGeneratorExpression> cge = ge.Parse(propertyValue);
				if (cmSystemTools::IsOn(cge->Evaluate(target->GetLocalGenerator(), configName)))
				{
					return true;
				}
			}
		}
		return false;
	}

	static void WriteAliases(GenerationContext& context,
		bool outputGlobals,
		const std::string& previousTargetName,
		std::string& finalTargetName)
	{
		context.fc.WriteSectionHeader(outputGlobals ? "Global Aliases" : "Target Aliases");

		// Write the following aliases:
		// Per Target
		// Per Config
		// All

		typedef std::map<std::string, std::vector<std::string> > TargetListMap;
		TargetListMap perConfig;
		std::vector<std::string> finalTargets;
		std::vector<std::string> targets;

		for (OrderedTargets::iterator targetIter = context.orderedTargets.begin(); 
			targetIter != context.orderedTargets.end();
			++targetIter)
		{
			const cmGeneratorTarget* constTarget = (*targetIter);
			if(constTarget->GetType() == cmState::INTERFACE_LIBRARY)
			{
				continue;
			}

			TargetContextMap::iterator findResult = context.targetContexts.find(constTarget);
			if (findResult == context.targetContexts.end())
			{
				std::ostringstream s;
				s << "No TargetContext found for target " << constTarget->GetName() << " in " << constTarget->LocalGenerator->GetCurrentSourceDirectory() << " ( 0x" << std::hex << (unsigned long long) constTarget << std::dec << " ) " << " ( 0x" << std::hex << (unsigned long long) constTarget->Target << std::dec << " ) " << std::endl;
				cmSystemTools::Error(s.str().c_str());
				cmSystemTools::SetFatalErrorOccured();
				continue;
			}

			cmGeneratorTarget* target = findResult->second.target;
			cmLocalFastbuildGenerator* lg = findResult->second.lg;
			const std::string & targetName = target->GetName();

			if (target->GetType() == cmState::GLOBAL_TARGET)
			{
				if (!outputGlobals)
					continue;
			}
			else
			{
				if (outputGlobals)
					continue;
			}

			targets.clear();
			bool isFinalTarget = false;
			std::vector<std::string>::const_iterator
				iter = context.self->GetConfigurations().begin(),
				end = context.self->GetConfigurations().end();
			for (; iter != end; ++iter)
			{
				const std::string & configName = *iter;
				std::string aliasName = targetName + "-" + configName;

				targets.push_back(aliasName);

				if (IsPartOfDefaultBuild(context, target, configName))
				{
					perConfig[configName].push_back(aliasName);
					isFinalTarget = true;
				}
			}
			if (isFinalTarget)
			{
				finalTargets.push_back(targetName);
			}

			context.fc.WriteCommand("Alias", "'" + targetName + "'");
			context.fc.WritePushScope();
			context.fc.WriteArray("Targets",
				Wrap(targets, "'", "'"));
			context.fc.WritePopScope();
		}

		context.fc.WriteSectionHeader(outputGlobals ? "Final Aliases" : "Config Aliases");

		if (!outputGlobals)
		{
			// alias of all non-global targets is ALL_BUILD
			finalTargetName = "ALL_BUILD";
		}
		else
		{
			if (finalTargets.empty())
			{
				finalTargetName = previousTargetName; // no new default target added, keep the existing target
			}
			else
			{
				finalTargetName = "ALL";
				std::vector<std::string>::const_iterator
					iter = finalTargets.begin(),
					end = finalTargets.end();
				for (; iter != end; ++iter)
				{
					finalTargetName += "_" + *iter;
				}
			}
		}
		if (!previousTargetName.empty())
		{
			// Add previousTargetName (ALL_BUILD)
			std::vector<std::string>::const_iterator
				iter = context.self->GetConfigurations().begin(),
				end = context.self->GetConfigurations().end();
			for (; iter != end; ++iter)
			{
				const std::string & configName = *iter;
				std::vector<std::string> & targets = perConfig[configName];
				targets.insert(targets.begin(), previousTargetName + "-" + configName);
			}
		}

		std::vector<std::string> aliasTargets;
		std::vector<std::string>::const_iterator
			iter = context.self->GetConfigurations().begin(),
			end = context.self->GetConfigurations().end();
		for (; iter != end; ++iter)
		{
			const std::string & configName = *iter;
			const std::vector<std::string> & targets = perConfig[configName];

			std::string aliasName = finalTargetName + '-' + configName;
			if (finalTargetName != previousTargetName)
			{
				context.fc.WriteCommand("Alias", Quote(aliasName));
				context.fc.WritePushScope();
				context.fc.WriteArray("Targets",
					Wrap(targets, "'", "'"));
				context.fc.WritePopScope();
			}

			if (outputGlobals)
			{
				aliasTargets.clear();
				aliasTargets.push_back(aliasName);
				context.fc.WriteCommand("Alias", Quote(configName));
				context.fc.WritePushScope();
				context.fc.WriteArray("Targets",
					Wrap(aliasTargets, "'", "'"));
				context.fc.WritePopScope();
			}
		}

		if (finalTargetName != previousTargetName)
		{
			context.fc.WriteCommand("Alias", Quote(finalTargetName));
			context.fc.WritePushScope();
			context.fc.WriteArray("Targets", 
				Wrap(context.self->GetConfigurations(), "'" + finalTargetName + "-", "'"));
			context.fc.WritePopScope();
		}
	}

	static void WriteVSConfigurations(GenerationContext& context)
	{
		context.fc.WriteSectionHeader("VisualStudio Project Generation");

		context.fc.WriteVariable("ProjectCommon", "");
		context.fc.WritePushScopeStruct();
		context.fc.WriteVariable("ProjectBuildCommand", Quote("cd ^$(SolutionDir) &amp; fbuild -vs -dist -cache -monitor ^$(ProjectName)-^$(Configuration)"));
		context.fc.WriteVariable("ProjectRebuildCommand", Quote("cd ^$(SolutionDir) &amp; fbuild -vs -dist -cache -monitor -clean ^$(ProjectName)-^$(Configuration)"));

		// detect platform (only for MSVC)
		std::string platformToolset;
		// Translate the cmake compiler id into the PlatformToolset
		cmMakefile* mf = context.root->GetMakefile();
		if (mf != 0)
		{
			// figure out which language to use
			// for now care only for C and C++
			std::string compilerVar = "CMAKE_CXX_COMPILER";
			if (context.self->GetLanguageEnabled("CXX") == false)
			{
				compilerVar = "CMAKE_C_COMPILER";
			}

			std::string compilerId = mf->GetSafeDefinition(compilerVar + "_ID");
			std::string compilerVersion = mf->GetSafeDefinition(compilerVar + "_VERSION");
			if (compilerId == "MSVC")
			{
				if (compilerVersion.compare(0, 3, "19.") != std::string::npos)
				{
					// Using vs2015 / vc14
					platformToolset = "v140";
				}
				else if (compilerVersion.compare(0, 3, "18.") != std::string::npos)
				{
					// Using vs2013 / vc12
					platformToolset = "v120";
				}
				else if (compilerVersion.compare(0, 3, "16.") != std::string::npos)
				{
					// Using vs2010 / vc10
					platformToolset = "v100";
				}
			}
		}
		if (!platformToolset.empty())
		{
			context.fc.WriteVariable("PlatformToolset", Quote(platformToolset));
		}
		context.fc.WritePopScope();

        const std::map<std::string, std::string> configAlias = context.self->GetVSConfigAlias();

		// Iterate over all configurations and define them:
		std::vector<std::string>::const_iterator
			iter = context.self->GetConfigurations().begin(),
			end = context.self->GetConfigurations().end();
		for (; iter != end; ++iter)
		{
			const std::string & configName = *iter;
			context.fc.WriteVariable("BaseProject_" + configName, "");
			context.fc.WritePushScopeStruct();

			// Using base config
			context.fc.WriteCommand("Using", ".ProjectCommon");

            std::map<std::string, std::string>::const_iterator vsconfigIter = configAlias.find(configName);
            const std::string& vsconfig = vsconfigIter == configAlias.end() ? configName : vsconfigIter->second;
			// Output platform (TODO: CURRENTLY ONLY HANDLED FOR WINDOWS)
			context.fc.WriteVariable("Platform", Quote(context.self->GetPlatformName()));
			context.fc.WriteVariable("Config", Quote(vsconfig));

			context.fc.WritePopScope();
		}

		// Write out a list of all Visual Studio project configs
		context.fc.WriteArray("ProjectConfigs",
			Wrap(context.self->GetConfigurations(), ".BaseProject_", ""));
	}

    static void WriteVSConfigAlias(
        GenerationContext& context, const std::string& targetName)
    {
        const std::map<std::string, std::string> configAlias = context.self->GetVSConfigAlias();
        for (std::map<std::string, std::string>::const_iterator
            iter = configAlias.begin(),
            end = configAlias.end();
            iter != end; ++iter)
        {
            std::vector<std::string> aliasTargets;
            aliasTargets.push_back(targetName + "-" + iter->first);
            context.fc.WriteCommand("Alias", Quote(targetName + "-" + iter->second));
            context.fc.WritePushScope();
            context.fc.WriteArray("Targets",
                Wrap(aliasTargets, "'", "'"));
            context.fc.WritePopScope();
        }
    }

	static void WriteVSProject(
		GenerationContext& context,
		cmGeneratorTarget& target)
	{
		const std::string& targetName = target.GetName();

        WriteVSConfigAlias(context, targetName);

		context.fc.WriteCommand("VCXProject",
			Quote(targetName + "-project"));
		context.fc.WritePushScope();

		context.fc.WriteVariable("ProjectOutput", Quote(target.GetSupportDirectory() + "/" + targetName + ".vcxproj"));

		std::vector<std::string> projectFiles;
		std::vector<cmSourceFile*> sources;
		for (std::vector<std::string>::const_iterator
			iter = context.self->GetConfigurations().begin(),
			end = context.self->GetConfigurations().end();
			iter != end; ++iter)
		{
			const std::string & configName = *iter;
			target.GetSourceFiles(sources, configName);

			context.fc.WriteVariable("Project_" + configName, "");
			context.fc.WritePushScopeStruct();

			// Using base config
			context.fc.WriteCommand("Using", ".BaseProject_" + configName);

			// Output platform (TODO: CURRENTLY ONLY HANDLED FOR WINDOWS)
			context.fc.WriteVariable("Target", Quote(targetName + "-" + configName));

			context.fc.WritePopScope();
		}
		context.fc.WriteArray("ProjectConfigs", Wrap(context.self->GetConfigurations(), ".Project_", ""));

		std::vector<cmSourceFile*>::const_iterator sourcesEnd
			= cmRemoveDuplicates(sources);
		for (std::vector<cmSourceFile*>::const_iterator si = sources.begin();
		si != sourcesEnd; ++si)
		{
			cmSourceFile* sf = *si;
			// don't add source files which have the GENERATED property set:
			if (sf->GetPropertyAsBool("GENERATED"))
			{
				continue;
			}
			std::string const& sfp = sf->GetFullPath();
			projectFiles.push_back(Quote(sfp));
		}

		context.fc.WriteArray("ProjectFiles", projectFiles);

		context.fc.WritePopScope();
	}

	static void WriteVSSolution(
		GenerationContext& context,
		const std::string& buildTargetName,
		const std::string& finalTargetName)
	{
		context.fc.WriteSectionHeader("VisualStudio Solution Generation");

		std::string rootDirectory;
		cmMakefile* mf = context.root->GetMakefile();
		if (mf != 0)
		{
			rootDirectory = mf->GetHomeOutputDirectory();
			rootDirectory += "/";
		}

        WriteVSConfigAlias(context, buildTargetName);
        context.fc.WriteCommand("VCXProject", Quote(buildTargetName + "-project"));
		context.fc.WritePushScope();
		context.fc.WriteVariable("ProjectOutput", Quote(rootDirectory + buildTargetName + ".vcxproj"));
		context.fc.WritePopScope();

		if (finalTargetName != buildTargetName)
		{
            WriteVSConfigAlias(context, finalTargetName);
			context.fc.WriteCommand("VCXProject", Quote(finalTargetName + "-project"));
			context.fc.WritePushScope();
			context.fc.WriteVariable("ProjectOutput", Quote(rootDirectory + finalTargetName + ".vcxproj"));
			context.fc.WritePopScope();
		}

		context.fc.WriteCommand("VSSolution", Quote("solution"));
		context.fc.WritePushScope();

		std::string topLevelSlnName = rootDirectory;
		topLevelSlnName += (mf != 0) ? context.root->GetProjectName() : context.self->GetName();
		topLevelSlnName += "-FASTBuild.sln";

		context.fc.WriteVariable("SolutionOutput", Quote(topLevelSlnName));

		//context.fc.WriteArray("SolutionConfigs", Wrap(context.self->GetConfigurations(), ".config_",""));
		context.fc.WriteVariable("SolutionConfigs", ".ProjectConfigs");

		bool useFolders = context.self->UseFolderProperty();

		std::map<std::string,std::vector<std::string> > targetsInFolder;

		std::string predefinedFolderName;
		if (useFolders) predefinedFolderName = context.self->GetPredefinedTargetsFolder();

		targetsInFolder[predefinedFolderName].push_back(Quote(buildTargetName + "-project"));
		if (finalTargetName != buildTargetName)
		{
			targetsInFolder[predefinedFolderName].push_back(Quote(finalTargetName + "-project"));
		}

		// Now iterate each target in order
		for (OrderedTargets::iterator targetIter = context.orderedTargets.begin();
		targetIter != context.orderedTargets.end();
			++targetIter)
		{
			const cmGeneratorTarget* constTarget = (*targetIter);
			if (constTarget->GetType() == cmState::INTERFACE_LIBRARY)
			{
				continue;
			}

			TargetContextMap::iterator findResult = context.targetContexts.find(constTarget);
			if (findResult == context.targetContexts.end())
			{
				std::ostringstream s;
				s << "No TargetContext found for target " << constTarget->GetName() << " in " << constTarget->LocalGenerator->GetCurrentSourceDirectory() << " ( 0x" << std::hex << (unsigned long long) constTarget << std::dec << " ) " << " ( 0x" << std::hex << (unsigned long long) constTarget->Target << std::dec << " ) " << std::endl;
				cmSystemTools::Error(s.str().c_str());
				cmSystemTools::SetFatalErrorOccured();
				continue;
			}

			cmGeneratorTarget* target = findResult->second.target;
			cmLocalFastbuildGenerator* lg = findResult->second.lg;

			std::string folder;
			if (useFolders)
			{
				const char* p = target->GetProperty("FOLDER");
				if (p)
				{
					folder = p;
				}
			}
			targetsInFolder[folder].push_back(Quote(target->GetName() + "-project"));
		}

		if (targetsInFolder.find("") != targetsInFolder.end())
		{
			context.fc.WriteArray("SolutionProjects", targetsInFolder[""]);
			targetsInFolder.erase("");
		}

		int folderCount = 1;
		std::vector<std::string> folders;
		for (std::map<std::string, std::vector<std::string> >::const_iterator iter = targetsInFolder.begin(), itend = targetsInFolder.end();
		iter != itend; ++iter)
		{
			const std::string& folderName = iter->first;
			const std::vector<std::string>& projects = iter->second;

			std::stringstream folderVarName;
			folderVarName << "folder_" << (folderCount++);

			std::string folderVarNameStr = folderVarName.str();

			context.fc.WriteVariable(folderVarNameStr, "");
			context.fc.WritePushScopeStruct();

			context.fc.WriteVariable("Path", Quote(folderName));
			context.fc.WriteArray("Projects", projects);

			context.fc.WritePopScope();

			folders.push_back("."+folderVarNameStr);
		}

		if (!folders.empty())
		{
			context.fc.WriteArray("SolutionFolders", folders);
		}

		context.fc.WriteVariable("SolutionBuildProject", Quote(finalTargetName + "-project"));

		context.fc.WritePopScope();
	}

};

//----------------------------------------------------------------------------
cmGlobalGeneratorFactory* cmGlobalFastbuildGenerator::NewFactory()
{
	return new cmGlobalGeneratorSimpleFactory<cmGlobalFastbuildGenerator>();
}

//----------------------------------------------------------------------------
cmGlobalFastbuildGenerator::cmGlobalFastbuildGenerator(cmake* cm)
: cmGlobalGenerator(cm)
{
	this->FindMakeProgramFile = "CMakeFastbuildFindMake.cmake";
	this->DefaultPlatformName = "Win32";
}

//----------------------------------------------------------------------------
cmGlobalFastbuildGenerator::~cmGlobalFastbuildGenerator()
{

}

//----------------------------------------------------------------------------
std::string cmGlobalFastbuildGenerator::GetName() const 
{
	return fastbuildGeneratorName; 
}

//----------------------------------------------------------------------------
bool cmGlobalFastbuildGenerator::SetGeneratorPlatform(std::string const& p, cmMakefile* mf)
{
	if (p == "Win32" || p == "x64" || p == "Itanium" || p == "ARM" || p == "")
	{
		this->GeneratorPlatform = p;
	}
	else
	{
		return this->cmGlobalGenerator::SetGeneratorPlatform(p, mf); // unrecognized platform
	}
	mf->AddDefinition("CMAKE_VS_PLATFORM_NAME", this->GetPlatformName().c_str());
	return this->cmGlobalGenerator::SetGeneratorPlatform("", mf);
}

//----------------------------------------------------------------------------
std::string const& cmGlobalFastbuildGenerator::GetPlatformName() const
{
	if (!this->GeneratorPlatform.empty())
	{
		return this->GeneratorPlatform;
	}
	return this->DefaultPlatformName;
}
//----------------------------------------------------------------------------
cmLocalGenerator *cmGlobalFastbuildGenerator::CreateLocalGenerator(cmMakefile* mf)
{
	cmLocalGenerator * lg = new cmLocalFastbuildGenerator(this, mf);
	return lg;
}

//----------------------------------------------------------------------------
void cmGlobalFastbuildGenerator::EnableLanguage(
	std::vector<std::string>const &  lang,
	cmMakefile *mf, bool optional)
{
	// Create list of configurations requested by user's cache, if any.
	this->cmGlobalGenerator::EnableLanguage(lang, mf, optional);
	Detail::Detection::DetectConfigurations(this, mf, this->Configurations,this->VSConfigAlias);
}

//----------------------------------------------------------------------------
void cmGlobalFastbuildGenerator::Generate()
{
	// Execute the standard generate process
	cmGlobalGenerator::Generate();

	Detail::Generation::GenerateRootBFF(this);
}

//----------------------------------------------------------------------------
void cmGlobalFastbuildGenerator::GenerateBuildCommand(
	std::vector<std::string>& makeCommand,
	const std::string& makeProgram,
	const std::string& projectName,
	const std::string& projectDir,
	const std::string& targetName,
	const std::string& config,
	bool /*fast*/, bool /*verbose*/,
	std::vector<std::string> const& /*makeOptions*/)
{
	// A build command for fastbuild looks like this:
	// fbuild.exe [make-options] [-config projectName.bff] <target>-<config>

	// Setup make options
	std::vector<std::string> makeOptionsSelected;

	// Select the caller- or user-preferred make program
	std::string makeProgramSelected =
		this->SelectMakeProgram(makeProgram);

	// Select the target
	std::string targetSelected = targetName;
	// Note an empty target is a valid target (defaults to ALL anyway)
	if (targetSelected == "clean")
	{
		makeOptionsSelected.push_back("-clean");
		targetSelected = "";
	}
  
	// Select the config
	std::string configSelected = config;
	if (configSelected.empty())
	{
		configSelected = "Debug";
	}

	// Select fastbuild target
	if (targetSelected.empty())
	{
		targetSelected = configSelected;
	}
	else
	{
		targetSelected += "-" + configSelected;
	}

	// Hunt the fbuild.bff file in the directory above
	std::string configFile;
	if (!cmSystemTools::FileExists(projectDir + "fbuild.bff"))
	{
		configFile = cmSystemTools::FileExistsInParentDirectories("fbuild.bff", projectDir.c_str(), "");
	}

	// Build the command
	makeCommand.push_back(makeProgramSelected);
	
	// Push in the make options
	makeCommand.insert(makeCommand.end(), makeOptionsSelected.begin(), makeOptionsSelected.end());

	/*
	makeCommand.push_back("-config");
	makeCommand.push_back(projectName + ".bff");
	*/

	makeCommand.push_back("-showcmds");
	makeCommand.push_back("-ide");

	if (!configFile.empty())
	{
		makeCommand.push_back("-config");
		makeCommand.push_back(configFile);
	}

	// Add the target-config to the command
	if (!targetSelected.empty())
	{
		makeCommand.push_back(targetSelected);
	}
}

//----------------------------------------------------------------------------
void cmGlobalFastbuildGenerator::AppendDirectoryForConfig(
	const std::string& prefix,
	const std::string& config,
	const std::string& suffix,
	std::string& dir)
{
	if(!config.empty())
	{
		dir += prefix;
		dir += config;
		dir += suffix;
	}
}

//----------------------------------------------------------------------------
void cmGlobalFastbuildGenerator::ComputeTargetObjectDirectory(
	cmGeneratorTarget* gt) const
{
	std::string dir = gt->LocalGenerator->GetCurrentBinaryDirectory();
	dir += "/";
	std::string tgtDir = gt->LocalGenerator->GetTargetDirectory(gt);
	if (!tgtDir.empty()) {
		dir += tgtDir;
		dir += "/";
	}
	const char* cd = this->GetCMakeCFGIntDir();
	if (cd && *cd) {
		dir += cd;
		dir += "/";
	}
	gt->ObjectDirectory = dir;
/*
	cmTarget* target = gt->Target;

	// Compute full path to object file directory for this target.
	std::string dir;
	dir += gt->Makefile->GetCurrentBinaryDirectory();
	dir += "/";
	dir += gt->LocalGenerator->GetTargetDirectory(gt);
	dir += "/";
	gt->ObjectDirectory = dir;
*/
}

//----------------------------------------------------------------------------
const char* cmGlobalFastbuildGenerator::GetCMakeCFGIntDir() const
{
	return FASTBUILD_DOLLAR_TAG "ConfigName" FASTBUILD_DOLLAR_TAG;
}

//----------------------------------------------------------------------------
void cmGlobalFastbuildGenerator::GetTargetSets(TargetDependSet& projectTargets,
					TargetDependSet& originalTargets,
					cmLocalGenerator* root, GeneratorVector const& gv)
{
	cmGlobalGenerator::GetTargetSets(projectTargets, originalTargets, root, gv);
}
//----------------------------------------------------------------------------
const std::vector<std::string> & cmGlobalFastbuildGenerator::GetConfigurations() const
{
	return Configurations;
}

const std::map<std::string,std::string> & cmGlobalFastbuildGenerator::GetVSConfigAlias() const
{
    return VSConfigAlias;
}

//----------------------------------------------------------------------------
