# -*- coding: utf-8 -*-
import matplotlib.pyplot as plt
import numpy as np
import sys,os
import random,subprocess
#versions = ['rpc','onesided']
colors=['b','g','r','c','m','y','k','grey']
algs = ['occ', 'nowait', 'waitdie', 'mvcc', 'sundial']
out_path = sys.argv[1]
apps = ['bank', 'ycsb', 'tpcc']
versions = ['rpc','onesided']
# versions = ['rpc','onesided', 'hybrid']
filenames = ["cor1","cor3","cor5","cor7","cor9","cor11","cor13","cor15","cor17" ,"cor19"]
# prefix = sys.argv[2]

for appname in apps:
    #force to use specifiy data folder
    if appname == 'bank' or appname == 'tpcc':
        prefix = 'finaldata/increasing_cor_num_bank/'
    else: # app is YCSB
        prefix = 'finaldata/increasing_cor_num/routine_28_with_calvin/'    

    filenames_w_path = []
    for i in range(len(filenames)):
        filenames_w_path.append(prefix + filenames[i])

    plt.figure(figsize=(15,4))
    for i, item in enumerate(algs):
            ax = plt.subplot(1,5,i+1)
            # ax = plt.subplot(3,2,i+1)
            name_list = ['read','lock&read','release write','renew_lease','commit']#,'overall'
            tput = [[],[],[]]
            lat = [[],[],[]]
            for x in range(2): # 3 version(rpc rdma)
                    # for y in range(5): # 5 protocol
                    tres = 0.0
                    lres = 0.0
                    for thename in filenames_w_path:
                            fname = thename + '/drtmh-nocc' + item + '-' + appname + '-4-' + versions[x] + '.log_0'
                            if os.path.isfile(fname):
                                    print(fname)
                                    res = subprocess.check_output(('awk', 'BEGIN{cnt=0; sum=0} /System throughput/{cnt+=1; if($5~"K") sum+=$4/1000.0; else if($5~"M") sum+=$4; } END{if (cnt == 0) print 0; else print sum/cnt}', fname))
                                    # print(res)
                                    tres = (float(res.decode('ascii').strip("\n")))
                                    f = open(fname)
                                    l = f.readline()
                                    while(l):
                                            if l.find("bench_listener2.cc:279") != -1:
                                                    res = float(l.split("average latency:")[1].strip("ms.\n"))
                                                    lres = res
                                                    break
                                            l = f.readline()
                            if(tres == 0.0 or lres == 0.0):
                                    continue
                            tput[x].append(tres)
                            lat[x].append(lres)
            width = 0.3
            objs = []
            if item == "sundial":
                    objs = ['read','lock&read','release write','renew_lease','commit']#,'overall'
            else:
                    objs = ['read','lock&read','release write','commit']#,'overall'
            # for x in range(3):
            #         while (len(num[x]) < 5):
            #                 num[x].append(0.0)
            #         if(item != "sundial"):
            #                 num[x][3] = -3.33
            #                 num[x].remove(-3.33)
                    # print(num)

            
            y_pos = np.arange(len(objs))*1.5
            rects_list=[]
            # for idx in range(len(versions)):
                    # print(num[idx])
                    # rects_list.append(plt.bar(y_pos+idx*width, num[idx], width, color=colors[idx], alpha=0.8, linewidth=0.1))
            myco = ['b','g','r']
            iternum = 2
            if item == "mvcc":
                iternum = 2
            for hkz in range(iternum):
                # x = [0,1,2,3,4,5,6]
                # y = [0.3,0.4,2,5,3,4.5,4]
                # for iterx in range(7):
                #     # x[iterx] = random.random()
                #     y[iterx] = random.random()
                # print(tput[hkz])
                # print(lat[hkz])
                # tput[hkz].sort()
                # lat[hkz].sort()
                rects_list.append(plt.plot(tput[hkz],lat[hkz], ls='-', c=myco[hkz], lw=2, marker='.', label=))

            plt.title(item, fontsize=8, loc='right')
            plt.ylabel('Latency', fontsize=8)
            plt.xlabel('Troughput', fontsize=8)
            # plt.title(apps[wl]+'_'+str(server_cnt), fontsize=8, loc='right')
            ax.get_xaxis().set_tick_params(direction='in', width=1, length=0)
            # plt.xticks(y_pos+width*3/2, objs, fontsize=6, rotation=0)
            ax.get_yaxis().set_tick_params(direction='in', width=0.5, length=2, pad=1)
            plt.yticks(fontsize=6)
            ax.yaxis.get_offset_text().set_size(2)
            ax.yaxis.set_ticks_position('left')
    
    # plt.legend((rects_list[0][0], rects_list[1][0], rects_list[2][0]), versions, fontsize=6, bbox_to_anchor=(-1.2, -0.2, 2, .06), loc=3, ncol=2, mode="expand", borderaxespad=0.)
    # plt.legend()
    plt.suptitle("Throughput-Latency " + appname)
    # plt.title('Interesting Graph',loc ='left')
# plt.show()
    plt.savefig(out_path + '/' + 'latency-tput' + appname + '.pdf', bbox_inches='tight')
