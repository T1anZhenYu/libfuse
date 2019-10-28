#include<iostream>
#include <string.h>
#include<stdlib.h>
using namespace std;


typedef struct seanet_inode{
	char self_eid[100];
	char root_manifest_eid[100];
	seanet_inode(){
		strcpy(self_eid,"");
		strcpy(root_manifest_eid,"");
	}
}seanet_inode;

typedef struct SEANET_FILE{
	char name[100];
	char eid[100];
	SEANET_FILE(){
		strcpy(name,"");
		strcpy(eid,"");
	}
}SEANET_FILE;
//���ڴ�����ɢ��SEANET_SEANET_FILE�ṹ��ָ��ƴ��һ���������ڴ档
//���룺�ṹ��ָ�룻�ṹ����Ŀ��
//���أ������ڴ��ָ�룻 
char* concat_data(SEANET_FILE** f,int length){
	char *content = (char*)malloc(length * sizeof(SEANET_FILE));
	strcpy(content,"");
	for(int i = 0;i < length;i++){
		strcat(content,(char*)f[i]);
	}
	return content ;
}
//�����ļ��е�eid������ȫ��·�������� 
void calculate_dir_eid(char* eid,const char* name){
	strcpy(eid,"");
	strcpy(eid,name);
}
//�����ļ���eid�������ļ����ݼ���
void calculate_file_eid(char* eid,const char* content){
	strcpy(eid,"");
	strcpy(eid,content);
}

int Seanetfs_sendfile(const char* filecontent,int flength,int arg, char* rootmfsteid){
	if(strlen(filecontent) == 0){
		strcpy(rootmfsteid,"NULL");		
	} else {
		strcpy(rootmfsteid,filecontent);			
	}
	return 1;
}
//mode = 0: dir; mode = 1: SEANET_FILE
//���룺inode ָ�룻����rootmanifest������SEANET_FILE�ṹ��ָ�룻 �ṹ���������inode���ͣ�0�ļ��У�1�ļ� 
void make_inode(seanet_inode *si,const char* dir_name,
			SEANET_FILE** files = NULL ,const char* file_content = NULL,int length = 0, int mode = 0){
	char* content;
	if(mode == 0){
		calculate_dir_eid(si->self_eid,dir_name);
		content = concat_data(files,length);
		int res = Seanetfs_sendfile(content,length,0,si->root_manifest_eid);		
	}else{
		calculate_file_eid(si->self_eid,file_content);
		int res = Seanetfs_sendfile(file_content,length,0,si->root_manifest_eid);		
	}
}

void parse_inode(char* inodeEid){
	seanet_inode *si = new seanet_inode();
	
}

int Seanetfs_sendinode(unsigned char inodeEid[20], char* inodechunk, int ilength){
	return 1;
}

int Seanetfs_getinode(unsigned char inodeEid[20], char* inodechunk){
	
}

int main (){
	const char* file_content = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
	
	SEANET_FILE *root = new SEANET_FILE;
	strcpy(root->name,"/root");
	calculate_dir_eid(root->eid,root->name);//��ʱrootmanifest eid Ϊ�ա�����rootΪ���ļ��С�
	seanet_inode * root_inode = new seanet_inode;
	make_inode(root_inode,"/root");
	cout<<"inode info"<<endl;
	cout<<root_inode->self_eid<<endl;
	cout<<root_inode->root_manifest_eid<<endl;
	
	SEANET_FILE* file1 = new SEANET_FILE;
	strcpy(file1->name,"/root/file1");
	calculate_file_eid(file1->eid,file_content);
	seanet_inode *file1_inode = new seanet_inode;
	make_inode(file1_inode,"/root/file1",NULL,file_content,0,1);
	
	
	
}



