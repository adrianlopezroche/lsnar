/*
	LSNAR Copyright (c) 2020 Adrian Lopez

	This software is provided 'as-is', without any express or implied
	warranty. In no event will the authors be held liable for any damages
	arising from the use of this software.

	Permission is granted to anyone to use this software for any purpose,
	including commercial applications, and to alter it and redistribute it
	freely, subject to the following restrictions:

	1. The origin of this software must not be misrepresented; you must not
	claim that you wrote the original software. If you use this software
	in a product, an acknowledgment in the product documentation would be
	appreciated but is not required.
	2. Altered source versions must be plainly marked as such, and must not be
	misrepresented as being the original software.
	3. This notice may not be removed or altered from any source distribution.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <limits.h>
#include <unistd.h>
#include <err.h>

struct cleanup_data
{
	FILE *f;
	void *sf;
} g_cleanup;

struct dumpdir_file
{
	char control_code;
	char *filename;
};

struct snar_directory
{
	long long nfs;
	long long timestamp_sec;
	long long timestamp_nsec;
	long long dev;
	long long ino;
	char *name;

	struct dumpdir_file *files;
	size_t num_files;
	size_t alloc_files;
};

struct snar_file
{
	int version;
	long long timestamp_sec;
	long long timestamp_nsec;
	struct snar_directory *directories;
	size_t num_directories;
	size_t alloc_directories;
};

struct directory_filter_flags
{
	char control_code_Y : 1;
	char control_code_N : 1;
	char control_code_D : 1;
	char empty : 1;
};

int match_string(FILE *file, const char *text)
{
	int i = 0;
	while (text[i] != '\0')
	{
		int c = fgetc(file);

		if (c != text[i++])
		{
			ungetc(c, file);
			return 0;
		}
	}

	return 1;
}

void expect_string(FILE *file, const char *text)
{
	if (!match_string(file, text))
		errx(1, "input file contains invalid characters");
}

int peek_null(FILE *file)
{
	int c = fgetc(file);

	ungetc(c, file);

	return c == 0;
}

void expect_null(FILE *file)
{
	if (!peek_null(file))
		errx(1, "input file contains invalid characters");

	fgetc(file);
}

char *read_string(FILE *file)
{
	size_t allocated = 0;
	size_t to_allocate = 128;

	char *s = malloc(to_allocate);
	if (s == 0)
		errx(1, "out of memory");

	allocated = to_allocate;

	int i = 0;

	s[i] = '\0';

	int c = fgetc(file);
	while (c != EOF)
	{
		if (i+1 >= allocated)
		{
			to_allocate = allocated * 2;

			char *snew = realloc(s, to_allocate);
			if (snew == 0)
				errx(1, "out of memory");

			allocated = to_allocate;

			s = snew;
		}

		s[i++] = c;

		if (c == '\0')
			break;

		c = fgetc(file);
	}

	ungetc(c, file);

	s[i] = '\0';

	return s;
}

int read_longlong(FILE *file, long long *value)
{
	char buf[21];
	int i = 0;

	int c = fgetc(file);
	while (c != EOF)
	{
		if (!isdigit(c))
			break;

		if (i+1 >= sizeof(buf))
			errx(1, "input string too long");

		buf[i++] = (char)c;

		c = fgetc(file);
	}

	if (c != EOF)
		ungetc(c, file);

	buf[i] = '\0';

	*value = atoll(buf);

	if (c == EOF || c == 0 || c == '\n')
		return 1;

	return 0;
}

void format_timestamp(char *buf_out, size_t buf_out_size, long long sec, long long nsec)
{
	char buf[20];

	if (nsec > 999999999)
		errx(1, "invalid timestamp detected");

	time_t tt;
	struct tm t;
	tt = (time_t)sec;
	localtime_r(&tt, &t);
	strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);

	snprintf(buf_out, buf_out_size, "%s.%09lld", buf, nsec);
}

void add_directory(struct snar_file *f, struct snar_directory *d)
{
	size_t to_alloc;
	struct snar_directory *new_dirs;

	if (f->alloc_directories <= f->num_directories)
	{
		if (f->alloc_directories == 0)
			to_alloc = 128;
		else
			to_alloc = f->alloc_directories * 2;

		new_dirs = realloc(f->directories, to_alloc * sizeof(struct snar_directory));
		if (!new_dirs)
			errx(1, "out of memory");

		f->directories = new_dirs;
		f->alloc_directories = to_alloc;
	}

	f->directories[f->num_directories++] = *d;
}

void add_file(struct snar_directory *d, struct dumpdir_file *f)
{
	size_t to_alloc;
	struct dumpdir_file *new_files;

	if (d->alloc_files <= d->num_files)
	{
		if (d->alloc_files == 0)
			to_alloc = 128;
		else
			to_alloc = d->alloc_files * 2;

		new_files = realloc(d->files, to_alloc * sizeof(struct dumpdir_file));
		if (!new_files)
			errx(1, "out of memory");

		d->files = new_files;
		d->alloc_files = to_alloc;
	}

	d->files[d->num_files++] = *f;
}

int filecmp(const void *a, const void *b)
{
	const struct dumpdir_file *fa = a;
	const struct dumpdir_file *fb = b;

	return strcmp(fa->filename, fb->filename);
}

int dircmp(const void *a, const void *b)
{
	const struct snar_directory *da = a;
	const struct snar_directory *db = b;

	return strcmp(da->name, db->name);
}

void sort_files(struct snar_directory *d)
{
	qsort(d->files, d->num_files, sizeof(struct dumpdir_file), filecmp);
}

void sort_snar(struct snar_file *sf)
{
	for (size_t d = 0; d < sf->num_directories; ++d)
		sort_files(&sf->directories[d]);

	qsort(sf->directories, sf->num_directories, sizeof(struct snar_directory), dircmp);
}

struct directory_filter_flags parse_directory_filter_flags(char *specifiers)
{
	struct directory_filter_flags result;

	result.control_code_Y = 0;
	result.control_code_N = 0;
	result.control_code_D = 0;
	result.empty = 0;

	size_t length = strlen(specifiers);

	for (int s = 0; s < length; ++s)
	{
		switch (specifiers[s])
		{
			case 'Y':
				result.control_code_Y = 1;
				break;

			case 'N':
				result.control_code_N = 1;
				break;

			case 'D':
				result.control_code_D = 1;
				break;

			case '0':
				result.empty = 1;
				break;

			default:
				errx(1, "invalid argument '%c' for '-t'\nvalid arguments are f, d, Y, N, D, 0\n", specifiers[s]);
				break;
		}
	}

	return result;
}

int match_control_code(char control_code, const struct directory_filter_flags *filter_flags)
{
	switch (control_code)
	{
		case 'Y':
			return filter_flags->control_code_Y;

		case 'N':
			return filter_flags->control_code_N;

		case 'D':
			return filter_flags->control_code_D;

		default:
			return 0;
	}
}

void print_file(struct dumpdir_file *file)
{
	printf("    File: %c %s\n", file->control_code, file->filename);
}

void print_directory(struct snar_directory *dir, const struct directory_filter_flags *filter_flags)
{
	if (dir->num_files == 0 && !filter_flags->empty)
		return;

	printf("%s\n\n", dir->name);
	printf("     NFS: %s\n", dir->nfs ? "Yes" : "No");

	char time_str[32];
	format_timestamp(time_str, sizeof(time_str), dir->timestamp_sec, dir->timestamp_nsec);
	printf("  Modify: %s\n", time_str);

	printf("     Dev: %lld\n", dir->dev);
	printf("   Inode: %lld\n", dir->ino);

	for (size_t f = 0; f < dir->num_files; ++f)
		print_file(&dir->files[f]);

	printf("\n");
}

void print_snar(struct snar_file *sf, const struct directory_filter_flags *filter_flags)
{
	for (size_t d = 0; d < sf->num_directories; ++d)
		print_directory(&sf->directories[d], filter_flags);
}

void snar_directory_free(struct snar_directory *directory)
{
	free(directory->name);

	for (size_t f = 0; f < directory->num_files; ++f)
		free(directory->files[f].filename);

	if (directory->files != 0)
		free(directory->files);
}

void snar_free(struct snar_file *sf)
{
	g_cleanup.sf = 0;

	for (size_t d = 0; d < sf->num_directories; ++d)
		snar_directory_free(sf->directories+d);

	if (sf->directories != 0)
		free(sf->directories);

	free(sf);
}

void read_file(struct dumpdir_file *f, FILE *file)
{
	f->control_code = fgetc(file);
	f->filename = read_string(file);

	expect_null(file); // end of file listing
}

void read_directory(struct snar_directory *d, FILE *file, const struct directory_filter_flags *filter_flags)
{
	d->files = 0;
	d->num_files = 0;
	d->alloc_files = 0;

	if (!read_longlong(file, &d->nfs) || !(d->nfs == 0 || d->nfs == 1))
		errx(1, "file contains invalid data");

	expect_null(file);

	if (!read_longlong(file, &d->timestamp_sec))
		errx(1, "file contains invalid data");

	expect_null(file);

	if (!read_longlong(file, &d->timestamp_nsec))
		errx(1, "file contains invalid data");

	expect_null(file);

	if (!read_longlong(file, &d->dev))
		errx(1, "file contains invalid data");

	expect_null(file);

	if (!read_longlong(file, &d->ino))
		errx(1, "file contains invalid data");

	expect_null(file);

	d->name = read_string(file);

	expect_null(file);

	if (!peek_null(file))
	{
		do
		{
			struct dumpdir_file f;

			read_file(&f, file);

			if (match_control_code(f.control_code, filter_flags))
				add_file(d, &f);
		} while (!peek_null(file) && !feof(file)); // more files?
	}

	expect_null(file); // end of dirdump
	expect_null(file); // end of directory
}

int read_snar_version(FILE *file)
{
	char tar_version[8192];
	int i = 0;

	if (!match_string(file, "GNU tar-"))
		return -1;

	int c = fgetc(file);

	while (c != EOF && c != '-')
	{
		tar_version[i++] = (char)c;

		if (c == '\r' || c == '\n' || (!isalnum(c) && !ispunct(c)))
			break;

		c = fgetc(file);
	}

	if (c != '-')
	{
		ungetc(c, file);
		return -1;
	}

	tar_version[i] = '\0';

	long long v;
	if (read_longlong(file, &v))
	{
		printf("Program Version: GNU tar-%s\n", tar_version);
		return (int)v;
	}

	return -1;
}

void read_snar(struct snar_file *sf, FILE *file)
{
	sf->directories = 0;
	sf->num_directories = 0;
	sf->alloc_directories = 0;

	sf->version = read_snar_version(file);
	if (sf->version == -1)
		errx(1, "unrecognized file format");

	printf(" Format Version: %d\n", sf->version);
	if (sf->version != 2)
		errx(1, "unsupported file format version");

	expect_string(file, "\n");

	if (!read_longlong(file, &sf->timestamp_sec))
		errx(1, "file contains invalid data");

	expect_null(file);

	if (!read_longlong(file, &sf->timestamp_nsec))
		errx(1, "file contains invalid data");

	char time_str[32];
	format_timestamp(time_str, sizeof(time_str), sf->timestamp_sec, sf->timestamp_nsec);
	printf("    Backup Time: %s\n\n", time_str);

	expect_null(file);
}

void do_cleanup()
{
	if (g_cleanup.f != 0)
		fclose(g_cleanup.f);

	if (g_cleanup.sf != 0)
		snar_free(g_cleanup.sf);
}

void print_help()
{
	printf("Usage: lsnar [options] SNAPSHOT_FILE\n\n");

	printf(" -s           sort files and directories in alphabetical order\n");
	printf(" -H           print only the snapshot file's header, omitting contents\n");
	printf(" -t [TYPE]    print only directory records containing entries of one or\n");
	printf("              more of the following TYPE values, concatenated together:\n");
	printf("                  Y - entries contained in the archive\n");
	printf("                  N - entries not contained in the archive\n");
	printf("                  D - entries that are directories\n");
	printf("                  0 - matches directory records containing no entries\n");
	printf("              the default behavior is equivalent to -tYND0\n");
	printf(" -h           displays this help message\n");

	printf("\n");
}

int main(int argc, char **argv)
{
	g_cleanup.f = 0;
	g_cleanup.sf = 0;
	atexit(do_cleanup);

	int sort_option = 0;
	int header_only_option = 0;
	struct directory_filter_flags filter_flags = parse_directory_filter_flags("fd0");

	tzset();

	int opt;
	while ((opt = getopt(argc, argv, "sHt:h")) !=  -1)
	{
		switch (opt)
		{
			case 's':
				sort_option = 1;
				break;
			case 'H':
				header_only_option = 1;
				break;
			case 't':
				filter_flags = parse_directory_filter_flags(optarg);
				break;
			case 'h':
				print_help();
				exit(0);
				break;
			default:
				printf("Try 'lsnar -h' for more information.\n");
				exit(1);
				break;
		}
	}

	if (optind == argc)
		errx(1, "no file specified");

	FILE *file = fopen(argv[optind], "rb");
	g_cleanup.f = file;

	if (file == 0)
		errx(1, "could not open file %s", argv[optind]);

	struct snar_file *sf = malloc(sizeof(struct snar_file));
	if (sf == 0)
		errx(1, "out of memory");

	g_cleanup.sf = sf;

	read_snar(sf, file);

	if (header_only_option)
		return 0;

	do
	{
		struct snar_directory d;

		read_directory(&d, file, &filter_flags);

		if (sort_option)
		{
			add_directory(sf, &d);
		}
		else
		{
			print_directory(&d, &filter_flags);
			snar_directory_free(&d);
		}
	} while (!peek_null(file) && !feof(file)); // more directories?

	g_cleanup.f = 0;
	fclose(file);

	if (sort_option)
	{
		sort_snar(sf);
		print_snar(sf, &filter_flags);
	}

	snar_free(sf);

	return 0;
}
