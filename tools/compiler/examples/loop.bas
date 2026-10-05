100 REM ============================================
110 REM TinyOS sample: BASIC line-numbered subset
120 REM Build: tcc loop.bas -o basic_demo.TNCR --lang basic
130 REM ============================================
140 PRINT "--- BASIC demo ---"
150 LET A = 0
160 LET B = 1
170 LET A = A + 1
180 LET B = B * 2
190 IF A < 6 THEN GOTO 170
200 PRINT "Loop ran 6 times, A ="
210 PRINT A
220 PRINT "2 to the 6th power, B ="
230 PRINT B
240 PRINT "GOTO version done."
250 END
