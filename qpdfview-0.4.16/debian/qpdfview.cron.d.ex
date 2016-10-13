#
# Regular cron jobs for the qpdfview package
#
0 4	* * *	root	[ -x /usr/bin/qpdfview_maintenance ] && /usr/bin/qpdfview_maintenance
