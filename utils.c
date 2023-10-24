/***************************

File: util.c

***************************/
static int    read_decimal (char *);
static double read_double (char *);
static int    read_hex (char *);
static int    read_hhmmss (char *);
static void   seconds_to_hhmmss (int, char **);
static int    test_in_string (char *, const char *);
static int    test_lead_string (char *, const char *);
static int    test_string (char *, const char *);
static int    test_tail_string (char *, const char *);
static void   write_binary ( int, char *, int );

/***********************************
*
*	Locate the first occurrence of a character in a string
*	Returns -1 if not found
*
***********************************/
static int locate_char (char a, char  * b) {
	char * c;
	int i;
	c = b;
	for (i=0;;i++) {
		if (*c==0) return -1;
		if (*c==a) return i;
		c++;
	}
}

/***********************************
*
*	Find the start of the next number after this number
*
***********************************/
static int next_number (char string[], int offset) {
	int retval = offset;
	while (string[retval]>='0' && string[retval]<='9') {
		retval++;
	}
	while (string[retval]>0 && (string[retval]<'0' || string[retval]>'9')) {
		retval++;
	}
//	printf("%d\n", retval);
	return retval;
}

/***********************************
*
*	Read and return a signed decimal number
*
***********************************/
static int read_decimal (char *string) {
    char *s;
    int retval = 0;
    int sign = 1;
    s = string;
    for (;;) {
        if (*s<1) break;
        if ('0'<=*s && *s<='9') break;
        if (*s=='-') {
            sign = -1;
            s++;
            break;
        }
        s++;
    }
    while (*s>='0' && *s<='9') {
//        printf("\r\nrd: %x",*s);
        retval = retval*10 + (*s) - '0';
        s++;
    }
//    s++;
    return sign * retval;
}

/***********************************
*
*	Read and return an unsigned real number as a double
*
*	NOTE: Does not read exponential forms
*
***********************************/
static double read_double (char *string) {
    char *s;
    double retval = 0.0;
    double fraction = 0.1;
    s = string;
    while (*s>0 && ( *s<'0'|| *s>'9' )) s++;
    while (*s>='0' && *s<='9') {
        retval = retval*10 + (*s) - '0';
        s++;
    }
    if (*s=='.') {
        s++;
        while (*s>='0' && *s<='9') {
            retval = retval + ((*s) - '0')*fraction;
            s++;
            fraction *= 0.1;
        }
    }
    return retval;
}

/***********************************
*
*	Read and return a hexadecimal number
*
***********************************/
static int read_hex (char *string) {
    char * s;
    int retval = 0;
    s = string;
    while (*s != '0') s++;
    s++;
    if (*s !='x') return 0;
    s++;
    while (('0' <= *s && *s <= '9') ||
        ('a' <= *s && *s <='f') ||
        ('A' <= *s && *s <= 'F')) {
        if (*s <= '9') {
             retval = retval * 16 + *s - '0';
        } else {
             retval = retval * 16 + ((*s) & 7) + 9;
        }
        s++;
    }
    return retval;
}

/***********************************
*
*	Read and return a time in hh:mm:ss format as seconds
*
***********************************/
static int read_hhmmss (char *string) {
    char *s;
    int seconds = 0;
    int retval = 0;
    s = string;
    while (*s>0 && ( *s<'0'|| *s>'9' )) s++;
    while (*s>='0' && *s<='9') {
        retval = retval*10 + (*s) - '0';
        s++;
    }
    /* Allow for the case of just seconds */
    if (*s!=':') {
        return retval;
    }
    /* Convert the number to minutes */
    retval *= 60;
    /* Here for mm:ss */
    s++;
    while (*s>='0' && *s<='9') {
        seconds = seconds*10 + (*s) - '0';
        s++;
    }
    if (*s!=':') {
        return retval+seconds;
    }
    /* Here for hh:mm:ss */
    retval = retval * 60 + seconds * 60;
    seconds = 0;
    s++;
    while (*s>='0' && *s<='9') {
        seconds = seconds*10 + (*s) - '0';
        s++;
    }
    return retval+seconds;
}

/***********************************
*
*	Write the given seconds into the output buffer provided
*
***********************************/
static void seconds_to_hhmmss (int seconds, char ** ptr_out) {
	int j;
	int ss = seconds % 60;
	int mm = ((seconds-ss)/60) % 60;
	int hh = (seconds - ss - mm*60) /3600;
	j = sprintf ((*ptr_out), "%02d:%02d:%02d ", hh, mm, ss);
	(*ptr_out) += j;
	while (**ptr_out!=0) {
		(*ptr_out)++;
	}
}

/***********************************
*
*	Test if String b forms part of String a.
*	Returns the offset it if exists, otherwise it returns -1
*
***********************************/
static int test_in_string (char *a, const char *b) {
	char *p;
	const char *q;
	p = a;
	q = b;
	while (*p!=0) {
		if (test_lead_string(p, q)) return (int) (p-a);
		p++;
	}
	return -1;
}

/***********************************
*
*	Test if String b matches the start of String a
*
***********************************/
static int test_lead_string (char *a, const char *b) {
	char *p;
	const char *q;
	p = a;
	q = b;
	for (;;) {
		if (*q == 0) return 1;
		if (*p != *q) return 0;
		p++;
		q++;
	}
	return 0;
}

/***********************************
*
*	Test if String b matches String a
*
***********************************/
static int test_string (char *a, const char *b) {
	char *p;
	const char *q;
	p = a;
	q = b;
	while (*p == *q) {
		if (*p == 0) {
			return 1;
		}
		p++;
		q++;
	}
	return 0;
}

/***********************************
*
*	Test if String b matches the end of String a
*
***********************************/
static int test_tail_string (char *a, const char *b) {
	char *p;
	const char *q;
	p = a;
	q = b;
	while (*p!=0) p++;
	while (*q!=0) q++;
	for (;;) {
		if (*p != *q) return 0;
		if (q == b) return 1;
		p--;
		q--;
	}
	return 0;
}

/***********************************
*
*	Write the first 'length' bits of 'n' to the string 'a'
*
***********************************/
static void write_binary ( int n, char *a, int length) {
	int i;
	char *p;
	int mask = 1;
	p = a;

	for ( i = 0; i < length; i++ ) {
		if ( ( n & mask ) != 0 ) {
			*p = '1';
		} else {
			*p = '0';
		}
		p++;
		mask <<= 1;
	}
}
