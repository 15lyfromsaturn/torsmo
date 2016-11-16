[NAME]
torsmo \- Tyopoyta ORvelo System MOnitor
[DESCRIPTION]
.\" Add any additional description here
[EXAMPLES]
.B
torsmo -t '${time %D %H:%m}' -o -u 30
.PP
Start torsmo in its own window with date and clock as text and 30 sec update interval.

.B
torsmo -a top_left -x 5 -y 500 -d
.PP
Start torsmo to background at coordinates (5, 500).
[FILES]
~/.torsmorc default configuration file
