#include "ui.h"

int main()
{
	return monitor::run([] {
		monitor::ProcSource source;
		return monitor::sample(source);
	});
}
