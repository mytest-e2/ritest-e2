#include <lib/base/cfile.h>
#include <lib/base/eerror.h>
#include <lib/base/eerroroutput.h>
#include <lib/base/elock.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <time.h>

#include <string>
#include <ansidebug.h>

extern ePtr<eErrorOutput> m_erroroutput;

#ifdef MEMLEAK_CHECK
AllocList *allocList;
pthread_mutex_t memLock =
	PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

void DumpUnfreed()
{
	AllocList::iterator i;
	unsigned int totalSize = 0;

	if(!allocList)
		return;

	CFile f("/tmp/enigma2_mem.out", "w");
	if (!f)
		return;
	size_t len = 1024;
	char *buffer = (char*)malloc(1024);
	for(i = allocList->begin(); i != allocList->end(); i++)
	{
		unsigned int tmp;
		fprintf(f, "%s\tLINE %d\tADDRESS %p\t%d unfreed\ttype %d (btcount %d)\n",
			i->second.file, i->second.line, (void*)i->second.address, i->second.size, i->second.type, i->second.btcount);
		totalSize += i->second.size;

		char **bt_string = backtrace_symbols( i->second.backtrace, i->second.btcount );
		for ( tmp=0; tmp < i->second.btcount; tmp++ )
		{
			if ( bt_string[tmp] )
			{
				char *beg = strchr(bt_string[tmp], '(');
				if ( beg )
				{
					std::string tmp1(beg+1);
					int pos1=tmp1.find('+'), pos2=tmp1.find(')');
					if ( pos1 != std::string::npos && pos2 != std::string::npos )
					{
						std::string tmp2(tmp1.substr(pos1,(pos2-pos1)));
						tmp1.erase(pos1);
						if (tmp1.length())
						{
							int state;
							abi::__cxa_demangle(tmp1.c_str(), buffer, &len, &state);
							if (!state)
								fprintf(f, "%d %s%s\n", tmp, buffer,tmp2.c_str());
							else
								fprintf(f, "%d %s\n", tmp, bt_string[tmp]);
						}
					}
				}
				else
					fprintf(f, "%d %s\n", tmp, bt_string[tmp]);
			}
		}
		free(bt_string);
		if (i->second.btcount)
			fprintf(f, "\n");
	}
	free(buffer);

	fprintf(f, "-----------------------------------------------------------\n");
	fprintf(f, "Total Unfreed: %d bytes\n", totalSize);
	fflush(f);
};
#endif

Signal2<void, int, const std::string&> logOutput;
int logOutputConsole = 1;
int logOutputColors = 1;

static bool inNoNewLine = false;

static pthread_mutex_t DebugLock =
	PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP;


static const char *alertToken[] = {
// !!! all strings must be written in lower case !!!
	"error",
	"fail",
	"not available",
	"no module",
	"no such file",
	"cannot",
	"invalid ",	//space after "invalid" to prevent false detect of "invalidate"
	"bad parameter",
	"not found",
	NULL		//end of list
};

static const char *warningToken[] = {
// !!! all strings must be written in lower case !!!
	"warning",
	"unknown",
	"not implemented",
	NULL		//end of list
};

bool findToken(char *src, const char **list)
{
	bool res = false;
	if(!src || !list)
		return res;

	char *tmp = new char[strlen(src)+1];
	if(!tmp)
		return res;
	int idx=0;
	do{
		tmp[idx] = tolower(src[idx]);
	}while(src[idx++]);

	for(idx=0; list[idx]; idx++)
	{
		if(strstr(tmp, list[idx]))
		{
			res = true;
			break;
		}
	}
	delete [] tmp;
	return res;
}

void removeAnsiEsc(char *src)
{
	char *dest = src;
	bool cut = false;
	for(; *src; src++)
	{
		if (*src == (char)0x1b) cut = true;
		if (!cut) *dest++ = *src;
		if (*src == 'm' || *src == 'K') cut = false;
	}
	*dest = '\0';
}

void removeAnsiEsc(char *src, char *dest)
{
	bool cut = false;
	for(; *src; src++)
	{
		if (*src == (char)0x1b) cut = true;
		if (!cut) *dest++ = *src;
		if (*src == 'm' || *src == 'K') cut = false;
	}
	*dest = '\0';
}

char *printtime(char buffer[], int size)
{
	struct tm loctime ;
	struct timeval tim;
	gettimeofday(&tim, NULL);
	localtime_r(&tim.tv_sec, &loctime);
	snprintf(buffer, size, "%02d:%02d:%02d.%03ld", loctime.tm_hour, loctime.tm_min, loctime.tm_sec, tim.tv_usec / 1000L);
	return buffer;
}

extern void bsodFatal(const char *component);

void _eFatal(const char *file, int line, const char *function, const char* fmt, ...)
{
	char timebuffer[32];
	char header[256];
	char buf[1024];
	char ncbuf[1024];
	printtime(timebuffer, sizeof(timebuffer));
	snprintf(header, sizeof(header), "%s%s %s:%d %s ", inNoNewLine?"\n":"", timebuffer, file, line, function);
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	removeAnsiEsc(buf, ncbuf);
	singleLock s(DebugLock);
	logOutput(lvlFatal, std::string(header) + std::string(ncbuf) + "\n");

	if (!logOutputColors)
		{
			if(m_erroroutput && m_erroroutput->eErrorOutput::pipe_fd[1] && m_erroroutput->eErrorOutput::threadrunning)
			{
				int n;
				char obuf[1024];
				snprintf(obuf, sizeof(obuf), "FATAL: %s%s\n", header, ncbuf);
				n=write(m_erroroutput->eErrorOutput::pipe_fd[1], obuf, strlen(obuf));
				if(n<0)
					fprintf(stderr, "[eerror] row %d error: %s\n", __LINE__,strerror(errno));
			}
			else
				fprintf(stderr, "FATAL: %s%s\n", header, ncbuf);
		}
	else
	{
		snprintf(header, sizeof(header),
					"%s"		/*newline*/
			ANSI_RED	"%s "		/*color of timestamp*/
			ANSI_GREEN	"%s:%d "	/*color of filename and linenumber*/
			ANSI_BGREEN	"%s "		/*color of functionname*/
			ANSI_BWHITE			/*color of debugmessage*/
			, inNoNewLine?"\n":"", timebuffer, file, line, function);
			if(m_erroroutput && m_erroroutput->eErrorOutput::pipe_fd[1] && m_erroroutput->eErrorOutput::threadrunning)
			{
				int n;
				char obuf[1024];
				snprintf(obuf, sizeof(obuf), "FATAL: %s%s\n"ANSI_RESET, header , buf);
				n=write(m_erroroutput->eErrorOutput::pipe_fd[1], obuf, strlen(obuf));
				if(n<0)
					fprintf(stderr, "[eerror] row %d error: %s\n", __LINE__,strerror(errno));
			}
			else
				fprintf(stderr, "FATAL: %s%s\n"ANSI_RESET, header , buf);
	}
	bsodFatal("enigma2");
	inNoNewLine = false;
}

#ifdef DEBUG
void _eDebug(const char *file, int line, const char *function, const char* fmt, ...)
{
	char flagstring[10];
	char timebuffer[32];
	char header[256];
	char buf[1024];
	char ncbuf[1024];
	bool is_alert = false;
	bool is_warning = false;

	printtime(timebuffer, sizeof(timebuffer));
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	removeAnsiEsc(buf, ncbuf);
	is_alert = findToken(ncbuf, alertToken);
	if(!is_alert)
		is_warning = findToken(ncbuf, warningToken);

	if(is_alert)
		snprintf(flagstring, sizeof(flagstring), "%s", "[ E ]");
	else if(is_warning)
		snprintf(flagstring, sizeof(flagstring), "%s", "[ W ]");
	else
		snprintf(flagstring, sizeof(flagstring), "%s", "[   ]");

	snprintf(header, sizeof(header), "%s%s %s %s:%d %s ", inNoNewLine?"\n":"", timebuffer, flagstring, file, line, function);
	singleLock s(DebugLock);
	logOutput(lvlDebug, std::string(header) + std::string(ncbuf) + "\n");
	if (logOutputConsole)
	{
		if (!logOutputColors)
		{
			if(m_erroroutput && m_erroroutput->eErrorOutput::pipe_fd[1] && m_erroroutput->eErrorOutput::threadrunning)
			{
				int n;
				char obuf[1024];
				snprintf(obuf, sizeof(obuf), "%s%s\n", header, ncbuf);
				n=write(m_erroroutput->eErrorOutput::pipe_fd[1], obuf, strlen(obuf));
				if(n<0)
					fprintf(stderr, "[eerror] row %d error: %s\n", __LINE__,strerror(errno));
			}
			else
				fprintf(stderr, "%s%s\n", header, ncbuf);
		}
		else
		{
			snprintf(header, sizeof(header),
						"%s"		/*newline*/
						"%s%s "		/*color of timestamp*/
				ANSI_GREEN	"%s:%d "	/*color of filename and linenumber*/
				ANSI_BGREEN	"%s "		/*color of functionname*/
				ANSI_BWHITE			/*color of debugmessage*/
				, inNoNewLine?"\n":"", is_alert?ANSI_BRED:is_warning?ANSI_BYELLOW:ANSI_WHITE, timebuffer, file, line, function);
			if(m_erroroutput && m_erroroutput->eErrorOutput::pipe_fd[1] && m_erroroutput->eErrorOutput::threadrunning)
			{
				int n;
				char obuf[1024];
				snprintf(obuf, sizeof(obuf), "%s%s\n"ANSI_RESET, header, buf);
				n=write(m_erroroutput->eErrorOutput::pipe_fd[1], obuf, strlen(obuf));
				if(n<0)
					fprintf(stderr, "[eerror] row %d error: %s\n", __LINE__,strerror(errno));
			}
			else
				fprintf(stderr, "%s%s\n"ANSI_RESET, header, buf);
		}
	}
	inNoNewLine = false;
}

void _eDebugNoNewLineStart(const char *file, int line, const char *function, const char* fmt, ...)
{
	char flagstring[10];
	char timebuffer[32];
	char header[256];
	char buf[1024];
	char ncbuf[1024];
	bool is_alert = false;
	bool is_warning = false;

	printtime(timebuffer, sizeof(timebuffer));
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	removeAnsiEsc(buf, ncbuf);
	is_alert = findToken(ncbuf, alertToken);
	if(!is_alert)
		is_warning = findToken(ncbuf, warningToken);

	if(is_alert)
		snprintf(flagstring, sizeof(flagstring), "%s", "< E >");
	else if(is_warning)
		snprintf(flagstring, sizeof(flagstring), "%s", "< W >");
	else
		snprintf(flagstring, sizeof(flagstring), "%s", "<   >");

	snprintf(header, sizeof(header), "%s%s %s %s:%d %s ", inNoNewLine?"\n":"", timebuffer, flagstring, file, line, function);
	singleLock s(DebugLock);
	logOutput(lvlDebug, std::string(header) + std::string(ncbuf));
	if (logOutputConsole)
	{
		if (!logOutputColors)
		{
			if(m_erroroutput && m_erroroutput->eErrorOutput::pipe_fd[1] && m_erroroutput->eErrorOutput::threadrunning)
			{
				int n;
				char obuf[1024];
				snprintf(obuf, sizeof(obuf), "%s%s", header, ncbuf);
				n=write(m_erroroutput->eErrorOutput::pipe_fd[1], obuf, strlen(obuf));
				if(n<0)
					fprintf(stderr, "[eerror] row %d error: %s\n", __LINE__,strerror(errno));
			}
			else
				fprintf(stderr, "%s%s", header, ncbuf);
		}
		else
		{
			snprintf(header, sizeof(header),
						"%s"		/*newline*/
						"%s%s "		/*color of timestamp*/
				ANSI_GREEN	"%s:%d "	/*color of filename and linenumber*/
				ANSI_BGREEN	"%s "		/*color of functionname*/
				ANSI_BWHITE			/*color of debugmessage*/
				, inNoNewLine?"\n":"", is_alert?ANSI_BRED:is_warning?ANSI_BYELLOW:ANSI_WHITE, timebuffer, file, line, function);
			if(m_erroroutput && m_erroroutput->eErrorOutput::pipe_fd[1] && m_erroroutput->eErrorOutput::threadrunning)
			{
				int n;
				char obuf[1024];
				snprintf(obuf, sizeof(obuf), "%s%s", header, buf);
				n=write(m_erroroutput->eErrorOutput::pipe_fd[1], obuf, strlen(obuf));
				if(n<0)
					fprintf(stderr, "[eerror] row %d error: %s\n", __LINE__,strerror(errno));
			}
			else
				fprintf(stderr, "%s%s", header, buf);
		}
	}
	inNoNewLine = true;
}

void eDebugNoNewLine(const char* fmt, ...)
{
	char buf[1024];
	char ncbuf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	removeAnsiEsc(buf, ncbuf);
	singleLock s(DebugLock);
	logOutput(lvlDebug, std::string(ncbuf));
	if (logOutputConsole)
	{
		if(m_erroroutput && m_erroroutput->eErrorOutput::pipe_fd[1] && m_erroroutput->eErrorOutput::threadrunning)
		{
			int n;
			char obuf[1024];
			snprintf(obuf, sizeof(obuf), "%s", logOutputColors? buf : ncbuf);
			n=write(m_erroroutput->eErrorOutput::pipe_fd[1], obuf, strlen(obuf));
			if(n<0)
				fprintf(stderr, "[eerror] row %d error: %s\n", __LINE__,strerror(errno));
		}
		else
			fprintf(stderr, "%s", logOutputColors? buf : ncbuf);
	}
}

void eDebugNoNewLineEnd(const char* fmt, ...)
{
	char buf[1024];
	char ncbuf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	removeAnsiEsc(buf, ncbuf);
	singleLock s(DebugLock);
	logOutput(lvlDebug, std::string(ncbuf) + "\n");
	if (logOutputConsole)
	{
		if(!logOutputColors)
		{
			if(m_erroroutput && m_erroroutput->eErrorOutput::pipe_fd[1] && m_erroroutput->eErrorOutput::threadrunning)
			{
				int n;
				char obuf[1024];
				snprintf(obuf, sizeof(obuf), "%s\n", ncbuf);
				n=write(m_erroroutput->eErrorOutput::pipe_fd[1], obuf, strlen(obuf));
				if(n<0)
					fprintf(stderr, "[eerror] row %d error: %s\n", __LINE__,strerror(errno));
			}
			else
				fprintf(stderr, "%s\n", ncbuf);
		}
		else
		{
			if(m_erroroutput && m_erroroutput->eErrorOutput::pipe_fd[1] && m_erroroutput->eErrorOutput::threadrunning)
			{
				int n;
				char obuf[1024];
				snprintf(obuf, sizeof(obuf), "%s\n"ANSI_RESET, buf);
				n=write(m_erroroutput->eErrorOutput::pipe_fd[1], obuf, strlen(obuf));
				if(n<0)
					fprintf(stderr, "[eerror] row %d error: %s\n", __LINE__,strerror(errno));
			}
			else
				fprintf(stderr, "%s\n"ANSI_RESET, buf);
		}
	}
	inNoNewLine = false;
}

void _eWarning(const char *file, int line, const char *function, const char* fmt, ...)
{
	char timebuffer[32];
	char header[256];
	char buf[1024];
	char ncbuf[1024];
	printtime(timebuffer, sizeof(timebuffer));
	snprintf(header, sizeof(header), "%s%s [!W!] %s:%d %s ", inNoNewLine?"\n":"", timebuffer, file, line, function);
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	removeAnsiEsc(buf, ncbuf);
	va_end(ap);
	singleLock s(DebugLock);
	logOutput(lvlWarning, std::string(header) + std::string(ncbuf) + "\n");
	if (logOutputConsole)
	{
		if (!logOutputColors)
		{
			if(m_erroroutput && m_erroroutput->eErrorOutput::pipe_fd[1] && m_erroroutput->eErrorOutput::threadrunning)
			{
				int n;
				char obuf[1024];
				snprintf(obuf, sizeof(obuf), "%s%s\n", header, ncbuf);
				n=write(m_erroroutput->eErrorOutput::pipe_fd[1], obuf, strlen(obuf));
				if(n<0)
					fprintf(stderr, "[eerror] row %d error: %s\n", __LINE__,strerror(errno));
			}
			else
				fprintf(stderr, "%s%s\n", header, ncbuf);
		}
		else
		{
			snprintf(header, sizeof(header),
						"%s"		/*newline*/
				ANSI_BYELLOW	"%s "	/*color of timestamp*/\
				ANSI_GREEN	"%s:%d "	/*color of filename and linenumber*/
				ANSI_BGREEN	"%s "		/*color of functionname*/
				ANSI_BWHITE			/*color of debugmessage*/
				, inNoNewLine?"\n":"", timebuffer, file, line, function);
			if(m_erroroutput && m_erroroutput->eErrorOutput::pipe_fd[1] && m_erroroutput->eErrorOutput::threadrunning)
			{
				int n;
				char obuf[1024];
				snprintf(obuf, sizeof(obuf), "%s%s\n"ANSI_RESET, header, buf);
				n=write(m_erroroutput->eErrorOutput::pipe_fd[1], obuf, strlen(obuf));
				if(n<0)
					fprintf(stderr, "[eerror] row %d error: %s\n", __LINE__,strerror(errno));
			}
			else
				fprintf(stderr, "%s%s\n"ANSI_RESET, header, buf);
		}
	}
	inNoNewLine = false;
}
#endif // DEBUG


void ePythonOutput(const char *file, int line, const char *function, const char *string)
{
#ifdef DEBUG
	char flagstring[10];
	char timebuffer[32];
	char header[256];
	char buf[2*1024];
	char ncbuf[2*1024];
	bool is_alert = false;
	bool is_warning = false;

	printtime(timebuffer, sizeof(timebuffer));
	if(strstr(file, "e2reactor.py") || strstr(file, "traceback.py"))
		is_alert = true;
	snprintf(buf, sizeof(buf), "%s", string);
	removeAnsiEsc(buf, ncbuf);
	is_alert |= findToken(ncbuf, alertToken);
	if(!is_alert)
		is_warning = findToken(ncbuf, warningToken);

	if(is_alert)
		snprintf(flagstring, sizeof(flagstring), "%s", "{ E }");
	else if(is_warning)
		snprintf(flagstring, sizeof(flagstring), "%s", "{ W }");
	else
		snprintf(flagstring, sizeof(flagstring), "%s", "{   }");

	if(line)
		snprintf(header, sizeof(header), "%s%s %s %s:%d %s ", inNoNewLine?"\n":"", timebuffer, flagstring, file, line, function);
	else
	{
		snprintf(flagstring, sizeof(flagstring), "%s", "{ D }");
		snprintf(header, sizeof(header), "%s%s %s ", inNoNewLine?"\n":"", timebuffer, flagstring);
	}
	singleLock s(DebugLock);
	logOutput(lvlWarning, std::string(header) + std::string(ncbuf));
	if (logOutputConsole)
	{
		if (!logOutputColors)
		{
			if(m_erroroutput && m_erroroutput->eErrorOutput::pipe_fd[1] && m_erroroutput->eErrorOutput::threadrunning)
			{
				int n;
				char obuf[1024];
				snprintf(obuf, sizeof(obuf), "%s%s", header, ncbuf);
				n=write(m_erroroutput->eErrorOutput::pipe_fd[1], obuf, strlen(obuf));
				if(n<0)
					fprintf(stderr, "[eerror] row %d error: %s\n", __LINE__,strerror(errno));
			}
			else
				fprintf(stderr, "%s%s", header, ncbuf);
		}
		else
		{
			if(line)
			{
				snprintf(header, sizeof(header),
							"%s"		/*newline*/
							"%s%s "		/*color of timestamp*/
					ANSI_CYAN	"%s:%d "	/*color of filename and linenumber*/
					ANSI_BCYAN	"%s "		/*color of functionname*/
					ANSI_BWHITE			/*color of debugmessage*/
					, inNoNewLine?"\n":"", is_alert?ANSI_BRED:is_warning?ANSI_BYELLOW:ANSI_WHITE, timebuffer, file, line, function);
			}
			else
			{
				snprintf(header, sizeof(header),
							"%s"		/*newline*/
							"%s%s "		/*color of timestamp*/
					ANSI_BWHITE			/*color of debugmessage*/
					, inNoNewLine?"\n":"", ANSI_MAGENTA, timebuffer);
			}

			if(m_erroroutput && m_erroroutput->eErrorOutput::pipe_fd[1] && m_erroroutput->eErrorOutput::threadrunning)
			{
				int n;
				char obuf[1024];
				snprintf(obuf, sizeof(obuf), "%s%s"ANSI_RESET, header, buf);
				n=write(m_erroroutput->eErrorOutput::pipe_fd[1], obuf, strlen(obuf));
				if(n<0)
					fprintf(stderr, "[eerror] row %d error: %s\n", __LINE__,strerror(errno));
			}
			else
				fprintf(stderr, "%s%s"ANSI_RESET, header, buf);
		}
	}
#endif
	inNoNewLine = false;
}

void eWriteCrashdump()
{
		/* implement me */
}
