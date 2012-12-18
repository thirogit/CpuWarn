#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <sys/utsname.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>	/* For STDOUT_FILENO, among others */
#include <sys/ioctl.h>
#include <dirent.h>
#include <boost/regex.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/fstream.hpp>

namespace fs = boost::filesystem;

struct stats_cpu {
	unsigned long long cpu_user		__attribute__ ((aligned (16)));
	unsigned long long cpu_nice		__attribute__ ((aligned (16)));
	unsigned long long cpu_sys		__attribute__ ((aligned (16)));
	unsigned long long cpu_idle		__attribute__ ((aligned (16)));
	unsigned long long cpu_iowait		__attribute__ ((aligned (16)));
	unsigned long long cpu_steal		__attribute__ ((aligned (16)));
	unsigned long long cpu_hardirq		__attribute__ ((aligned (16)));
    unsigned long long cpu_softirq		__attribute__ ((aligned (16)));
	unsigned long long cpu_guest		__attribute__ ((aligned (16)));
	unsigned long long cpu_guest_nice	__attribute__ ((aligned (16)));
};

struct stats_range {
    double low;
    double high;
};


#define STATS_CPU_SIZE	(sizeof(struct stats_cpu))
#define MAX_PF_NAME		1024
#define SYSFS_DEVCPU		"/sys/devices/system/cpu"
#define STAT			"/proc/stat"
#define UPTIME			"/proc/uptime"
#define USBLED_BUS_DIR  "/sys/bus/usb/drivers/usbled"
/* Number of seconds per day */
#define SEC_PER_DAY	3600 * 24

/*
 * Macros used to display statistics values.
 *
 * NB: Define SP_VALUE() to normalize to %;
 * HZ is 1024 on IA64 and % should be normalized to 100.
 */
#define S_VALUE(m,n,p)	(((double) ((n) - (m))) / (p) * HZ)
#define SP_VALUE(m,n,p)	(((double) ((n) - (m))) / (p) * 100)


unsigned long long uptime[3] = {0, 0, 0};
unsigned long long uptime0[3] = {0, 0, 0};

/* NOTE: Use array of _char_ for bitmaps to avoid endianness problems...*/
//unsigned char *cpu_bitmap;	/* Bit 0: Global; Bit 1: 1st proc; etc. */

/* Structures used to store stats */
struct stats_cpu *st_cpu[3];

//struct tm mp_tstamp[3];


/* Interval and count parameters */
long interval = -1;

/* Nb of processors on the machine */
int cpu_nr = 0;

struct stats_range red_range = {80.0, 100.0};
struct stats_range blue_range = {50.0, 80.0};
struct stats_range green_range = {0.0, 50.0};

/*
 ***************************************************************************
 * Get local date and time.
 *
 * IN:
 * @d_off	Day offset (number of days to go back in the past).
 *
 * OUT:
 * @rectime	Current local date and time.
 *
 * RETURNS:
 * Value of time in seconds since the Epoch.
 ***************************************************************************
 */
time_t get_localtime(struct tm *rectime, int d_off)
{
	time_t timer;
	struct tm *ltm;

	time(&timer);
	timer -= SEC_PER_DAY * d_off;
	ltm = localtime(&timer);

	*rectime = *ltm;
	return timer;
}


/*
 ***************************************************************************
 * Compute time interval.
 *
 * IN:
 * @prev_uptime	Previous uptime value in jiffies.
 * @curr_uptime	Current uptime value in jiffies.
 *
 * RETURNS:
 * Interval of time in jiffies.
 ***************************************************************************
 */
unsigned long long get_interval(unsigned long long prev_uptime,
				unsigned long long curr_uptime)
{
	unsigned long long itv;

	/* prev_time=0 when displaying stats since system startup */
	itv = curr_uptime - prev_uptime;
	
	if (!itv) {	/* Paranoia checking */
		itv = 1;
	}

	return itv;
}

unsigned int get_HZ(void)
{
	long ticks;

	if ((ticks = sysconf(_SC_CLK_TCK)) == -1) {
		perror("sysconf");
	}

    return (unsigned int) ticks;
}

int get_sys_cpu_nr(void)
{
	DIR *dir;
	struct dirent *drd;
	struct stat buf;
	char line[MAX_PF_NAME];
	int proc_nr = 0;

	/* Open relevant /sys directory */
	if ((dir = opendir(SYSFS_DEVCPU)) == NULL)
		return 0;

	/* Get current file entry */
	while ((drd = readdir(dir)) != NULL) {

		if (!strncmp(drd->d_name, "cpu", 3) && isdigit(drd->d_name[3])) {
			snprintf(line, MAX_PF_NAME, "%s/%s", SYSFS_DEVCPU, drd->d_name);
			line[MAX_PF_NAME - 1] = '\0';
			if (stat(line, &buf) < 0)
				continue;
			if (S_ISDIR(buf.st_mode)) {
				proc_nr++;
			}
		}
	}

	/* Close directory */
	closedir(dir);

	return proc_nr;
}

int get_proc_cpu_nr(void)
{
	FILE *fp;
	char line[16];
	int num_proc, proc_nr = -1;

	if ((fp = fopen(STAT, "r")) == NULL) {
		fprintf(stderr, "Cannot open %s: %s\n", STAT, strerror(errno));
		exit(1);
	}

	while (fgets(line, 16, fp) != NULL) {

		if (strncmp(line, "cpu ", 4) && !strncmp(line, "cpu", 3)) {
			sscanf(line + 3, "%d", &num_proc);
			if (num_proc > proc_nr) {
				proc_nr = num_proc;
			}
		}
	}

	fclose(fp);

	return (proc_nr + 1);
}

int get_cpu_nr(unsigned int max_nr_cpus)
{
	int cpu_nr;

	if ((cpu_nr = get_sys_cpu_nr()) == 0) {
		/* /sys may be not mounted. Use /proc/stat instead */
		cpu_nr = get_proc_cpu_nr();
	}

	if (cpu_nr > max_nr_cpus) {
		fprintf(stderr, "Cannot handle so many processors!\n");
		exit(1);
	}

	return cpu_nr;
}


/*
 ***************************************************************************
 * Read machine uptime, independently of the number of processors.
 *
 * OUT:
 * @uptime	Uptime value in jiffies.
 ***************************************************************************
 */
void read_uptime(unsigned long long *uptime)
{
	FILE *fp;
	char line[128];
	unsigned long up_sec, up_cent;

	if ((fp = fopen(UPTIME, "r")) == NULL)
		return;

	if (fgets(line, 128, fp) == NULL) {
		fclose(fp);
		return;
	}

    unsigned int HZ = get_HZ();

	sscanf(line, "%lu.%lu", &up_sec, &up_cent);
	*uptime = (unsigned long long) up_sec * HZ +
	          (unsigned long long) up_cent * HZ / 100;

	fclose(fp);

}


/*
 ***************************************************************************
 * Read CPU statistics and machine uptime.
 *
 * IN:
 * @st_cpu	Structure where stats will be saved.
 * @nbr		Total number of CPU (including cpu "all").
 *
 * OUT:
 * @st_cpu	Structure with statistics.
 * @uptime	Machine uptime multiplied by the number of processors.
 * @uptime0	Machine uptime. Filled only if previously set to zero.
 ***************************************************************************
 */
void read_stat_cpu(struct stats_cpu *st_cpu, int nbr,
		   unsigned long long *uptime, unsigned long long *uptime0)
{
	FILE *fp;
	struct stats_cpu *st_cpu_i;
	struct stats_cpu sc;
	char line[8192];
	int proc_nb;

	if ((fp = fopen(STAT, "r")) == NULL) {
		fprintf(stderr, "Cannot open %s: %s\n", STAT, strerror(errno));
		exit(2);
	}

	while (fgets(line, 8192, fp) != NULL) {

		if (!strncmp(line, "cpu ", 4)) {

			/*
			 * All the fields don't necessarily exist,
			 * depending on the kernel version used.
			 */
			memset(st_cpu, 0, STATS_CPU_SIZE);

			/*
			 * Read the number of jiffies spent in the different modes
			 * (user, nice, etc.) among all proc. CPU usage is not reduced
			 * to one processor to avoid rounding problems.
			 */
			sscanf(line + 5, "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
			       &st_cpu->cpu_user,
			       &st_cpu->cpu_nice,
			       &st_cpu->cpu_sys,
			       &st_cpu->cpu_idle,
			       &st_cpu->cpu_iowait,
			       &st_cpu->cpu_hardirq,
			       &st_cpu->cpu_softirq,
			       &st_cpu->cpu_steal,
			       &st_cpu->cpu_guest,
			       &st_cpu->cpu_guest_nice);

			/*
			 * Compute the uptime of the system in jiffies (1/100ths of a second
			 * if HZ=100).
			 * Machine uptime is multiplied by the number of processors here.
			 *
			 * NB: Don't add cpu_guest/cpu_guest_nice because cpu_user/cpu_nice
			 * already include them.
			 */
			*uptime = st_cpu->cpu_user + st_cpu->cpu_nice    +
				st_cpu->cpu_sys    + st_cpu->cpu_idle    +
				st_cpu->cpu_iowait + st_cpu->cpu_hardirq +
				st_cpu->cpu_steal  + st_cpu->cpu_softirq;
		}

		else if (!strncmp(line, "cpu", 3)) {
			if (nbr > 1) {
				/* All the fields don't necessarily exist */
				memset(&sc, 0, STATS_CPU_SIZE);
				/*
				 * Read the number of jiffies spent in the different modes
				 * (user, nice, etc) for current proc.
				 * This is done only on SMP machines.
				 */
				sscanf(line + 3, "%d %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
				       &proc_nb,
				       &sc.cpu_user,
				       &sc.cpu_nice,
				       &sc.cpu_sys,
				       &sc.cpu_idle,
				       &sc.cpu_iowait,
				       &sc.cpu_hardirq,
				       &sc.cpu_softirq,
				       &sc.cpu_steal,
				       &sc.cpu_guest,
				       &sc.cpu_guest_nice);

				if (proc_nb < (nbr - 1)) {
					st_cpu_i = st_cpu + proc_nb + 1;
					*st_cpu_i = sc;
				}
				/*
				 * else additional CPUs have been dynamically registered
				 * in /proc/stat.
				 */

				if (!proc_nb && !*uptime0) {
					/*
					 * Compute uptime reduced to one proc using proc#0.
					 * Done if /proc/uptime was unavailable.
					 *
					 * NB: Don't add cpu_guest/cpu_guest_nice because cpu_user/cpu_nice
					 * already include them.
					 */
					*uptime0 = sc.cpu_user + sc.cpu_nice  +
						sc.cpu_sys     + sc.cpu_idle  +
						sc.cpu_iowait  + sc.cpu_steal +
						sc.cpu_hardirq + sc.cpu_softirq;
				}
			}
		}
	}

	fclose(fp);
}


/*
 ***************************************************************************
 * SIGALRM signal handler
 *
 * IN:
 * @sig	Signal number. Set to 0 for the first time, then to SIGALRM.
 ***************************************************************************
 */
void alarm_handler(int sig)
{
	signal(SIGALRM, alarm_handler);
	alarm(interval);
}

/*
 ***************************************************************************
 * Allocate stats structures and cpu bitmap.
 *
 * IN:
 * @nr_cpus	Number of CPUs. This is the real number of available CPUs + 1
 * 		because we also have to allocate a structure for CPU 'all'.
 ***************************************************************************
 */
void salloc_mp_struct(int nr_cpus)
{
	int i;

	for (i = 0; i < 3; i++) {

		if ((st_cpu[i] = (struct stats_cpu *) malloc(STATS_CPU_SIZE * nr_cpus))
		    == NULL) {
			perror("malloc");
			exit(4);
		}
		memset(st_cpu[i], 0, STATS_CPU_SIZE * nr_cpus);
		
	}	
}

/*
 ***************************************************************************
 * Free structures and bitmap.
 ***************************************************************************
 */
void sfree_mp_struct(void)
{
	int i;

	for (i = 0; i < 3; i++) {

		if (st_cpu[i]) {
			free(st_cpu[i]);
		}		
	}


}

double ll_sp_value(unsigned long long value1, unsigned long long value2,
		   unsigned long long itv)
{
	if ((value2 < value1) && (value1 <= 0xffffffff))
		/* Counter's type was unsigned long and has overflown */
		return ((double) ((value2 - value1) & 0xffffffff)) / itv * 100;
	else
		return SP_VALUE(value1, value2, itv);
}

/***************************************************************************
 * Since ticks may vary slightly from CPU to CPU, we'll want
 * to recalculate itv based on this CPU's tick count, rather
 * than that reported by the "cpu" line. Otherwise we
 * occasionally end up with slightly skewed figures, with
 * the skew being greater as the time interval grows shorter.
 *
 * IN:
 * @scc	Current sample statistics for current CPU.
 * @scp	Previous sample statistics for current CPU.
 *
 * RETURNS:
 * Interval of time based on current CPU.
 ***************************************************************************
 */
unsigned long long get_per_cpu_interval(struct stats_cpu *scc,
					struct stats_cpu *scp)
{
	unsigned long long ishift = 0LL;
	
	if ((scc->cpu_user - scc->cpu_guest) < (scp->cpu_user - scp->cpu_guest)) {
		/*
		 * Sometimes the nr of jiffies spent in guest mode given by the guest
		 * counter in /proc/stat is slightly higher than that included in
		 * the user counter. Update the interval value accordingly.
		 */
		ishift += (scp->cpu_user - scp->cpu_guest) -
		          (scc->cpu_user - scc->cpu_guest);
	}
	if ((scc->cpu_nice - scc->cpu_guest_nice) < (scp->cpu_nice - scp->cpu_guest_nice)) {
		/*
		 * Idem for nr of jiffies spent in guest_nice mode.
		 */
		ishift += (scp->cpu_nice - scp->cpu_guest_nice) -
		          (scc->cpu_nice - scc->cpu_guest_nice);
	}
	
	/*
	 * Don't take cpu_guest and cpu_guest_nice into account
	 * because cpu_user and cpu_nice already include them.
	 */
	return ((scc->cpu_user    + scc->cpu_nice   +
		 scc->cpu_sys     + scc->cpu_iowait +
		 scc->cpu_idle    + scc->cpu_steal  +
		 scc->cpu_hardirq + scc->cpu_softirq) -
		(scp->cpu_user    + scp->cpu_nice   +
		 scp->cpu_sys     + scp->cpu_iowait +
		 scp->cpu_idle    + scp->cpu_steal  +
		 scp->cpu_hardirq + scp->cpu_softirq) +
		 ishift);
}

/*
 ***************************************************************************
 *
 * IN:
 * @prev	Position in array where statistics used	as reference are.
 *		Stats used as reference may be the previous ones read, or
 *		the very first ones when calculating the average.
 * @curr	Position in array where statistics for current sample are.
  ***************************************************************************
 */
double compute_idle_stat(int prev, int curr)
{

    unsigned long long  g_itv;

	/* Compute time interval */
	g_itv = get_interval(uptime[prev], uptime[curr]);



    return
           (st_cpu[curr]->cpu_idle < st_cpu[prev]->cpu_idle) ?
           0.0 :  ll_sp_value(st_cpu[prev]->cpu_idle,st_cpu[curr]->cpu_idle, g_itv);



}


/*
 ***************************************************************************
 *
 * IN:
 * @curr	Position in array where statistics for current sample are.
 * @dis		TRUE if a header line must be printed.
 ***************************************************************************
 */
double get_idle_stat(int curr)
{
    return compute_idle_stat(!curr, curr);
}

void fix_stats(int curr)
{
    int prev = !curr;
    struct stats_cpu *scc, *scp;
    /* Fix CPU counter values for every offline CPU */
    for (int cpu = 1; cpu <= cpu_nr; cpu++) {

        scc = st_cpu[curr] + cpu;
        scp = st_cpu[prev] + cpu;

        if ((scc->cpu_user    + scc->cpu_nice + scc->cpu_sys   +
             scc->cpu_iowait  + scc->cpu_idle + scc->cpu_steal +
             scc->cpu_hardirq + scc->cpu_softirq) == 0) {
            /*
             * Offline CPU found.
             * Set current struct fields (which have been set to zero)
             * to values from previous iteration. Hence their values won't
             * jump from zero when the CPU comes back online.
             */
            *scc = *scp;
        }
    }
}

fs::path find_port_dir(fs::path usbDriverDir)
{
    boost::regex re("\\d-\\d:\\d\\.\\d");
    if(fs::exists(usbDriverDir) && fs::is_directory(usbDriverDir))
    {
        fs::directory_iterator end;
        fs::directory_iterator dirIt(usbDriverDir);
        for(;dirIt != end;dirIt++)
        {
            if(boost::regex_match(dirIt->path().leaf().string(),re))
            {
                return dirIt->path();
            }
        }

    }
    return fs::path();

}

void write_color(const char* color,const char* data)
{
    fs::path usbPortPath = find_port_dir(fs::path(USBLED_BUS_DIR));
    if(!usbPortPath.empty())
    {
        fs::path colorPath = usbPortPath / fs::path(color);
        if(fs::exists(colorPath))
        {

            fs::fstream colorFile(colorPath);
            colorFile << data;
            colorFile.close();
            //printf("COLOR EXISTS\n");
        }
    }

}

void switch_color(const char* color,bool sw)
{
    write_color(color, sw ? "1" : "0");
}

void switch_red(bool sw)
{
    switch_color("red",sw);
}

void switch_blue(bool sw)
{
    switch_color("blue",sw);
}

void switch_green(bool sw)
{
    switch_color("green",sw);
}

bool is_in_range(struct stats_range* range,double value)
{
    //printf("is_in_range: low=%.2f,high=%.2f,value=%.2f\n",range->low,range->high,value);
    return range->low <= value &&  value <= range->high;
}


void switch_indicator(double cpu_usage)
{
    /*printf("red_range=");
    print_range(&red_range);
    printf("\nblue_range=");
    print_range(&blue_range);
    printf("\ngreen_range=");
    print_range(&green_range);
    printf("\ncpu_usage=%.2f\n",cpu_usage);*/


    switch_red(is_in_range(&red_range,cpu_usage));
    switch_blue(is_in_range(&blue_range,cpu_usage));
    switch_green(is_in_range(&green_range,cpu_usage));
}

void print_range(struct stats_range* range)
{
    printf("%.2f-%.2f",range->low,range->high);
}



void loop()
{
	struct stats_cpu *scc;
	int cpu;
    int curr = 1;
    double idle_stat,usage_stat;


	/* Dont buffer data if redirected to a pipe */
	setbuf(stdout, NULL);

	/* Read stats */
	if (cpu_nr > 1) {
		/*
		 * Init uptime0. So if /proc/uptime cannot fill it,
		 * this will be done by /proc/stat.
		 */
		uptime0[0] = 0;
		read_uptime(&(uptime0[0]));
	}
	read_stat_cpu(st_cpu[0], cpu_nr + 1, &(uptime[0]), &(uptime0[0]));


	/* Set a handler for SIGALRM */
	alarm_handler(0);

	/* Save the first stats collected. Will be used to compute the average */
	uptime[2] = uptime[0];
	uptime0[2] = uptime0[0];
	memcpy(st_cpu[2], st_cpu[0], STATS_CPU_SIZE * (cpu_nr + 1));
	

	pause();

	do {
		/*
		 * Resetting the structure not needed since every fields will be set.
		 * Exceptions are per-CPU structures: Some of them may not be filled
		 * if corresponding processor is disabled (offline). We set them to zero
		 * to be able to distinguish between offline and tickless CPUs.
		 */
		for (cpu = 1; cpu <= cpu_nr; cpu++) {
			scc = st_cpu[curr] + cpu;
			memset(scc, 0, STATS_CPU_SIZE);
		}


		/* Read stats */
		if (cpu_nr > 1) {
			uptime0[curr] = 0;
			read_uptime(&(uptime0[curr]));
		}
		read_stat_cpu(st_cpu[curr], cpu_nr + 1, &(uptime[curr]), &(uptime0[curr]));


        idle_stat = get_idle_stat(curr);
        fix_stats(curr);
        usage_stat = 100.0 - idle_stat;
        printf("usage = %.2f%%\n",usage_stat);

        switch_indicator(usage_stat);

        pause();

	}
    while (1);


}


int main(int argc, char **argv)
{

    /* Get HZ */
	get_HZ();

	/* How many processors on this machine ? */
	cpu_nr = get_cpu_nr(~0);
	
	/*
	 * cpu_nr: a value of 2 means there are 2 processors (0 and 1).
	 * In this case, we have to allocate 3 structures: global, proc0 and proc1.
	 */
    salloc_mp_struct(cpu_nr + 1);

	interval = 3;
	
	/* Main loop */
    loop();

	/* Free structures */
	sfree_mp_struct();

	return 0;
}
