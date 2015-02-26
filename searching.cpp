

int findfast(unsigned char* buffer, unsigned long buflen, unsigned char* pattern, int patlen)
{
	unsigned int skip[256];
	int i,j;
	int patlen_min_1 = patlen-1;
	int buflen_min_1 = buflen-1;
	//initialize skip table
	for(i=0; i<256; i++) {
		skip[i] = patlen;
	}
	for(i=0; i<patlen; i++) {
		skip[pattern[i]] = patlen_min_1-i;
	}
	//search
	i = j = patlen_min_1;
	do {
		if (buffer[i] == pattern[j]) {
			i--;
			j--;
		} else {
			i = i+patlen-j;
			j = patlen_min_1;
			if (skip[buffer[i]] > (buflen-j)) {
				i = i+skip[buffer[i]]-(buflen-j);
			}
		}
	} while (j>=0 && i<buflen_min_1);
	return i+1;
}