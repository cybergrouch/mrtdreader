/*
 *   mrtdreader.c - Main program 
 *
 *   Copyright (C) 2014 Ruben Undheim <ruben.undheim@gmail.com>
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <nfc/nfc.h>
#include <signal.h>
#include "mrtd.h"

#define MAX_DEVICE_COUNT 5

static nfc_context *context = NULL;
static nfc_device *pnd = NULL;

static char *pn = NULL;
static char *dob = NULL;
static char *eov = NULL;
static char *extra_argument = NULL;

static char done = 0;

static void closedown(int sig)
{
	done = 1;
	printf("Stopping...\n");
}
static void forcestop(int sig)
{
	done = 1;
	printf("Stopping...\n");
	if(pnd != NULL)
		nfc_close(pnd);
	if(context != NULL)
		nfc_exit(context);
	exit(-1);
}

void printhex(char *description, uint8_t *input, int length)
{
	int i;
	printf("%s: ",description);
	for(i=0;i<length;i++){
		printf("%02x",input[i]);
	}
	printf("\n");
}

int parse_cmdline(int argc, char **argv)
{
	int s;
	opterr = 0;
	while((s = getopt(argc, argv, "p:b:e:")) != -1) {
		switch (s) {
			case 'p':
				pn = malloc(strlen(optarg)+1);
				memcpy(pn,optarg,strlen(optarg));
				break;
			case 'b':
				dob = malloc(strlen(optarg)+1);
				memcpy(dob,optarg,strlen(optarg));
				break;
			case 'e':
				eov = malloc(strlen(optarg)+1);
				memcpy(eov,optarg,strlen(optarg));
				break;
			case '?':
				if(optopt == 'p')
					fprintf(stderr, "Option -%c requires an argument.\n",optopt);
				else if(optopt == 'b')
					fprintf(stderr, "Option -%c requires an argument.\n",optopt);
				else if(optopt == 'e')
					fprintf(stderr, "Option -%c requires an argument.\n",optopt);
				else if(isprint(optopt)) 
					fprintf(stderr, "Unknown option '-%c'.\n",optopt);
				return -1;
			default:
				abort();
		}
	}

	if(argc == (optind + 1)){
		extra_argument = argv[optind];
	}
	if((extra_argument == NULL) && ((pn == NULL) || (dob == NULL) || (eov == NULL))){
		return -1;
	}

	return 0;
}

int main(int argc, char **argv)
{
	int i,ret,res;
	if(parse_cmdline(argc,argv) == -1){
		fprintf(stderr,"Usage: %s [options] <MRZ>\n",argv[0]);
		goto failed;
	}
	uint8_t *kmrz;
	uint8_t buffer[25];
	int inlength;
	if(extra_argument != NULL){
		inlength = strlen(extra_argument);
		if(inlength == 24){
			kmrz = extra_argument;
		}
		else if(inlength == 44){
			mrtd_bac_get_kmrz_from_mrz(extra_argument, buffer);
			kmrz = buffer;
		}
		else{
			fprintf(stderr,"Did not recognize <MRZ>\n");
			goto failed;
		}
	}
	else {
		mrtd_bac_get_kmrz(pn, dob, eov, buffer);
		kmrz = buffer;
	}
	if(pn != NULL) {
		free(pn);
		pn = NULL;
	}
	if(dob != NULL){
		free(dob);
		dob = NULL;
	}
	if(eov != NULL){
		free(eov);
		eov = NULL;
	}


	signal(SIGINT, forcestop);
	srand(time(NULL));

	nfc_init(&context);
	nfc_target ant;

	if(context == NULL){
		fprintf(stderr,"Unable to init libnfc (malloc)\n");
		goto failed;
	}
	nfc_connstring connstrings[MAX_DEVICE_COUNT];
	size_t szDeviceFound = nfc_list_devices(context,connstrings,MAX_DEVICE_COUNT);

	if(szDeviceFound == 0){
		fprintf(stderr,"No NFC device found.\n");
		goto failed;
	}

	pnd = nfc_open(context, connstrings[0]);
	if(pnd == NULL){
		fprintf(stderr,"Unable to open NFC device: %s\n",connstrings[0]);
		goto failed;
	}
	if(nfc_initiator_init(pnd) < 0){
		nfc_perror(pnd,"nfc_initiator_init");
		goto failed;
	}

	printf("NFC device: %s opened\n",nfc_device_get_name(pnd));

	if (nfc_device_set_property_bool(pnd, NP_INFINITE_SELECT, false) < 0){
		fprintf(stderr, "error setting property\n");
		goto failed;
	}

	nfc_modulation nmA,nmB;
	nmA.nmt = NMT_ISO14443A;
	nmA.nbr = NBR_106;
	nmB.nmt = NMT_ISO14443B;
	nmB.nbr = NBR_106;

	signal(SIGINT, closedown);
	while(1){
		ret = nfc_initiator_select_passive_target(pnd,nmA,NULL,0,&ant);
		if(ret > 0 || done) break;
		ret = nfc_initiator_select_passive_target(pnd,nmB,NULL,0,&ant);
		if(ret > 0 || done) break;
	}
	if(done)
		goto failed;
	printf("Target found!\n");
	signal(SIGINT, forcestop);

	uint8_t txbuffer[300];
	int txlen;
	uint8_t rxbuffer[300];
	int rxlen;

	uint64_t ssc_long;
	uint8_t ksenc[16];
	uint8_t ksmac[16];

	mrtd_bac_randomize_rndifd_kifd();
	ret = mrtd_bac_keyhandshake(pnd,kmrz,ksenc,ksmac,&ssc_long);

	if(ret == RET_CHALLENGE_FAILED){
		fprintf(stderr,"======================\nChallenge failed...\n======================\n");
		goto failed;
	}
	else if(ret < 0){
		goto failed;
	}
	printf("======================\nChallenge successful!\n======================\n");

	printf("\n");
	uint8_t *filecontent = NULL;
	filecontent = malloc(50000);
	int filecontentlength;


	printf("Getting EF.COM...");
	fflush(stdout);
	mrtd_fileread_read(pnd,"\x01\x1e",filecontent,&filecontentlength,ksenc,ksmac,&ssc_long);
	printf(" done\n");

	printhex("File content",filecontent,filecontentlength);
	printf("File size: %d\n",filecontentlength);

	uint8_t datagroups[20];
	int ndatagroups;
	mrtd_fileread_decode_ef_com(filecontent,filecontentlength,datagroups,&ndatagroups);

	printf("\n");

	printf("Getting EF.SOD...");
	fflush(stdout);
	mrtd_fileread_read(pnd,"\x01\x1d",filecontent,&filecontentlength,ksenc,ksmac,&ssc_long);
	printf(" done\n");

	printhex("File content",filecontent,filecontentlength);
	printf("File size: %d\n",filecontentlength);

	printf("\n");

	printf("Getting EF.DG1...");
	fflush(stdout);
	mrtd_fileread_read(pnd,"\x01\x01",filecontent,&filecontentlength,ksenc,ksmac,&ssc_long);
	printf(" done\n");

	filecontent[filecontentlength] = 0;
	printf("%s\n\n",filecontent);
	printf("File size: %d\n",filecontentlength);

	printf("\n");
	printf("Getting EF.DG2 which contains the image...");
	fflush(stdout);
	mrtd_fileread_read(pnd,"\x01\x02",filecontent,&filecontentlength,ksenc,ksmac,&ssc_long);
	printf(" done\n");


	mrtd_fileread_write_image_to_file(filecontent, filecontentlength, "image");

	nfc_close(pnd);
	nfc_exit(context);
	if(filecontent != NULL)
		free(filecontent);
	return 0;

	failed:
		if(pnd != NULL)
			nfc_close(pnd);
		if(context != NULL)
			nfc_exit(context);
		if(pn != NULL)
			free(pn);
		if(dob != NULL)
			free(dob);
		if(eov != NULL)
			free(eov);
		if(filecontent != NULL)
			free(filecontent);
		return -1;
}

