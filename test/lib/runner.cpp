/* -*- C++ -*- copyright (c) 2014 Red Hat, Inc.
 *
 * This file is part of LVM2.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "io.h"
#include "journal.h"
#include "filesystem.h"

#include <iostream>
#include <vector>
#include <deque>
#include <map>
#include <sstream>
#include <cassert>
#include <algorithm>

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h> /* rusage */
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/klog.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>

pid_t kill_pid = 0;
bool fatal_signal = false;
bool interrupt = false;

struct Options {
	bool verbose, batch, interactive, cont;
	std::string testdir, outdir;
	std::vector< std::string > flavours;
	Options() : verbose( false ), batch( false ), interactive( false ), cont( false ) {}
};

struct TestProcess
{
	std::string filename;
	bool interactive;
	int fd;

	void exec() {
		assert( fd >= 0 );
		if ( !interactive ) {
			close( STDIN_FILENO );
			dup2( fd, STDOUT_FILENO );
			dup2( fd, STDERR_FILENO );
			close( fd );
		}

		environment();

		setpgid( 0, 0 );

		execlp( "bash", "bash", "-noprofile", "-norc", filename.c_str(), NULL );
		perror( "execlp" );
		_exit( 202 );
	}

	void environment() {
		/* if (strchr(f, ':')) {
			strcpy(flavour, f);
			*strchr(flavour, ':') = 0;
			setenv("LVM_TEST_FLAVOUR", flavour, 1);
			strcpy(script, strchr(f, ':') + 1);
		} else {
			strcpy(script, f);
		} */
	}

	TestProcess( std::string file )
		: filename( file ), interactive( false ), fd( -1 )
	{}
};

struct TestCase {
	TestProcess child;
	std::string name, flavour;
	IO io;
	BufSink *iobuf;

	struct rusage usage;
	int status;
	bool timeout;
	pid_t pid;

	time_t start, end, silent_start;
	Options options;

	Journal *journal;

	std::string pretty() {
		return "[" + flavour + "] " + name;
	}

	std::string id() {
		return flavour + ":" + name;
	}

	void pipe() {
		int fds[2];

		if (socketpair( PF_UNIX, SOCK_STREAM, 0, fds )) {
			perror("socketpair");
			exit(201);
		}

		if (fcntl( fds[0], F_SETFL, O_NONBLOCK ) == -1) {
			perror("fcntl on socket");
			exit(202);
		}

		io.fd = fds[0];
		child.fd = fds[1];
		child.interactive = options.interactive;
	}

	bool monitor() {
		end = time( 0 );
		if ( wait4(pid, &status, WNOHANG, &usage) != 0 ) {
			io.sync();
			return false;
		}

		/* kill off tests after a minute of silence */
		if ( end - silent_start > 60 ) {
			kill( pid, SIGINT );
			sleep( 5 ); /* wait a bit for a reaction */
			if ( waitpid( pid, &status, WNOHANG ) == 0 ) {
				system( "echo t > /proc/sysrq-trigger" );
				kill( -pid, SIGKILL );
				waitpid( pid, &status, 0 );
			}
			timeout = true;
			io.sync();
			return false;
		}

		struct timeval wait;
		fd_set set;

		FD_ZERO( &set );
		FD_SET( io.fd, &set );
		wait.tv_sec = 0;
		wait.tv_usec = 500000; /* timeout 0.5s */

		if ( !options.verbose && !options.interactive && !options.batch )
			progress( Update ) << tag( "running" ) << pretty() << " " << end - start << std::flush;

		if ( select( io.fd + 1, &set, NULL, NULL, &wait ) > 0 )
			silent_start = end; /* something happened */

		usleep(500000);
		io.sync();

		return true;
	}

	std::string tag( std::string n ) {
		int pad = (12 - n.length());
		return "### " + std::string( pad, ' ' ) + n + ": ";
	}

	std::string tag( Journal::R r ) {
		std::stringstream s;
		s << r;
		return tag( s.str() );
	}

	enum P { First, Update, Last };

	std::ostream &progress( P p = Last )
	{
	        static struct : std::streambuf {} buf;
		static std::ostream null(&buf);

		if ( isatty( STDOUT_FILENO ) ) {
			if ( p != First )
				return std::cout << "\r";
			return std::cout;
		}

		if ( p == Last )
			return std::cout;
		return null;
	}

	void parent()
	{
		::close( child.fd );
		setupIO();

		journal->started( id() );
		silent_start = start = time( 0 );

		progress( First ) << tag( "running" ) << pretty() << std::flush;
		if ( options.verbose || options.interactive )
			progress() << std::endl;

		while ( monitor() );

		Journal::R r = Journal::UNKNOWN;

		if ( timeout ) {
			r = Journal::TIMEOUT;
		} else if ( WIFEXITED( status ) ) {
			if ( WEXITSTATUS( status ) == 0 )
				r = Journal::PASSED;
			else if ( WEXITSTATUS( status ) == 200 )
				r = Journal::SKIPPED;
			else
				r = Journal::FAILED;
		} else if ( interrupt && WIFSIGNALED( status ) && WTERMSIG( status ) == SIGINT )
			r = Journal::INTERRUPTED;
		else
			r = Journal::FAILED;

		io.close();

		/*
		if ((fd_debuglog = open(testdirdebug, O_RDONLY)) != -1) {
			drain(fd_debuglog, unlimited ? INT32_MAX : 4 * 1024 * 1024);
			close(fd_debuglog);
		} */

		if ( iobuf && r == Journal::FAILED || r == Journal::TIMEOUT )
			iobuf->dump( std::cout );

		journal->done( id(), r );
		progress( Last ) << tag( r ) << pretty() << std::endl;
		io.clear();
	}

	void run() {
		pipe();
		pid = kill_pid = fork();
		if (pid < 0) {
			perror("Fork failed.");
			exit(201);
		} else if (pid == 0) {
			io.close();
			chdir( options.testdir.c_str() );
			setenv("LVM_TEST_FLAVOUR", flavour.c_str(), 1);
			child.exec();
		} else {
			parent();
		}
	}

	void setupIO() {
		iobuf = 0;
		if ( options.verbose )
			io.sinks.push_back( new FdSink( 1 ) );
		else
			io.sinks.push_back( iobuf = new BufSink() );

		std::string n = id();
		std::replace( n.begin(), n.end(), '/', '_' );
		std::string fn = options.outdir + "/" + n + ".txt";
		io.sinks.push_back( new FileSink( fn ) );
	}

	TestCase( Journal &j, Options opt, std::string path, std::string name, std::string flavour )
		: timeout( false ), child( path ), name( name ), flavour( flavour ), options( opt ), journal( &j )
	{
	}
};

struct Main {
	bool die;
	time_t start;

	typedef std::vector< TestCase > Cases;
	typedef std::vector< std::string > Flavours;

	Journal journal;
	Options options;
	Cases cases;

	void setup() {
		Listing l = listdir( options.testdir, true );
		std::sort( l.begin(), l.end() );

		for ( Flavours::iterator flav = options.flavours.begin();
		      flav != options.flavours.end(); ++flav ) {

			for ( Listing::iterator i = l.begin(); i != l.end(); ++i ) {
				if ( i->substr( i->length() - 3, i->length() ) != ".sh" )
					continue;
				if ( i->substr( 0, 4 ) == "lib/" )
					continue;
				cases.push_back( TestCase( journal, options, options.testdir + *i, *i, *flav ) );
				cases.back().options = options;
			}
		}

		if ( options.cont )
			journal.read();
	}

	void run() {
		setup();
		start = time( 0 );
		std::cerr << "running " << cases.size() << " tests" << std::endl;

		for ( Cases::iterator i = cases.begin(); i != cases.end(); ++i ) {

			if ( options.cont && journal.done( i->id() ) )
				continue;

			i->run();

			if ( time(0) - start > 3 * 3600 ) {
				std::cerr << "3 hours passed, giving up..." << std::endl;
				die = 1;
			}

			if ( die || fatal_signal )
				break;
		}

		journal.banner();
		journal.write( options.outdir + "/list" );
		fsync_name( options.outdir + "/list" );
		if ( die || fatal_signal )
			exit( 1 );
	}

	Main( Options o ) : die( false ), options( o ), journal( o.outdir ) {}
};

static void handler( int sig ) {
	signal( sig, SIG_DFL ); /* die right away next time */
	if ( kill_pid > 0 )
		kill( -kill_pid, sig );
	fatal_signal = true;
	if ( sig == SIGINT )
		interrupt = true;
}

void setup_handlers() {
	/* set up signal handlers */
	for ( int i = 0; i <= 32; ++i )
		switch (i) {
			case SIGCHLD: case SIGWINCH: case SIGURG:
			case SIGKILL: case SIGSTOP: break;
			default: signal(i, handler);
		}
}

static int64_t get_time_us(void)
{
       struct timeval tv;

       (void) gettimeofday(&tv, 0);
       return (int64_t) tv.tv_sec * 1000000 + (int64_t) tv.tv_usec;
}


static const char *duration(time_t start, const struct rusage *usage)
{
	static char buf[100];
	int t = (int)(time(NULL) - start);

	int p = sprintf(buf, "%2d:%02d walltime", t / 60, t % 60);

	if (usage)
		sprintf(buf + p, "   %2ld:%02ld.%03ld u, %ld:%02ld.%03ld s, %5ldk rss, %8ld/%ld IO",
			usage->ru_utime.tv_sec / 60, usage->ru_utime.tv_sec % 60,
			usage->ru_utime.tv_usec / 1000,
			usage->ru_stime.tv_sec / 60, usage->ru_stime.tv_sec % 60,
			usage->ru_stime.tv_usec / 1000,
			usage->ru_maxrss / 1024,
			usage->ru_inblock, usage->ru_oublock);

	return buf;
}

struct Args {
	typedef std::vector< std::string > V;
	V args;

	Args( int argc, char **argv ) {
		for ( int i = 1; i < argc; ++ i )
			args.push_back( argv[ i ] );
	}

	bool has( std::string fl ) {
		return std::find( args.begin(), args.end(), fl ) != args.end();
	}

	std::string opt( std::string fl ) {
		V::iterator i = std::find( args.begin(), args.end(), fl );
		if ( i == args.end() || i + 1 == args.end() )
			return "";
		return *(i + 1);
	}
};

bool hasenv( const char *name ) {
	const char *v = getenv( name );
	if ( !v )
		return false;
	if ( strlen( v ) == 0 || !strcmp( v, "0" ) )
		return false;
	return true;
}

int main(int argc, char **argv)
{
	Args args( argc, argv );
	Options opt;

	if ( args.has( "--continue" ) )
		opt.cont = true;

	if ( args.has( "--batch" ) || hasenv( "BATCH" ) ) {
		opt.verbose = false;
		opt.batch = true;
	}

	if ( args.has( "--verbose" ) || hasenv( "VERBOSE" ) ) {
		opt.batch = false;
		opt.verbose = true;
	}

	if ( args.has( "--interactive" ) || hasenv( "INTERACTIVE" ) ) {
		opt.verbose = false;
		opt.batch = false;
		opt.interactive = true;
	}

	if ( args.has( "--flavours" ) ) {
		std::stringstream ss( args.opt( "--flavours" ) );
		std::string item;
		while ( std::getline( ss, item, ',' ) )
			opt.flavours.push_back( item );
	} else
		opt.flavours.push_back( "vanilla" );

	opt.outdir = args.opt( "--outdir" );
	opt.testdir = args.opt( "--testdir" );

	if ( opt.testdir.empty() )
		opt.testdir = "/usr/share/lvm2-testsuite";

	opt.testdir += "/";

	setup_handlers();

	Main main( opt );
	main.run();

}
