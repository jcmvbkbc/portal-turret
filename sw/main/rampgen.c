#include <stdio.h>
#include <math.h>

int main(void)
{
	int i;
	for (i = 0; i < 1024; ++i) {
		int v = (1 - cos(i * 3.1416 / 1024)) / 4 * 215 + 40;
		printf("%3d, %3d,%c", 0, v, (i + 1) % 16 ? ' ' : '\n');
	}
	printf("\n");
	for (i = 1023; i >=0; --i) {
		int v = (1 - cos(i * 3.1416 / 1024)) / 4 * 215 + 40;
		printf("%3d, %3d,%c", 0, v, i % 16 ? ' ' : '\n');
	}
	return 0;
}
