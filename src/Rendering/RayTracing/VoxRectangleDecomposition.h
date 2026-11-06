#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace mikan::vox {
struct SurfaceRectangle {int x,y,width,height;uint32_t label;};

// Independent C++ implementation of the reflex-chord / maximum-independent-set
// construction described at https://github.com/bWFuanVzYWth/rectangle-decomposition.
// Unlike the reference's fixed 64x64 scratch, process a whole VOX plane (<=256)
// without tile seams. Zero is background; equal nonzero labels may merge.
// Scratch is heap-backed, local to a bake, and reusable between planes.
class RectangleDecomposer {
    struct Run {int begin,end;uint32_t label;};
    struct Chord {int fixed,begin,end;};
    struct PathNode {int left,viaRight;};
    std::vector<Run> previousRuns,currentRuns;
    std::vector<Chord> horizontal,vertical;
    std::vector<std::vector<int>> starts,ends,queries;
    std::vector<int> activeHorizontal,edges,leftPair,rightPair,distance,queue,cursors;
    std::vector<int> offsets,writeOffsets;
    std::vector<uint8_t> seenLeft,seenRight,horizontalCuts,verticalCuts;
    std::vector<PathNode> path;
    std::vector<SurfaceRectangle> active,nextActive,rowRuns;

    void ExtractChords(const std::vector<uint32_t>& labels,int width,int height,bool transpose) {
        const int lines=transpose?width:height,length=transpose?height:width;
        previousRuns.clear();
        auto pixel=[&](int coordinate,int line){return transpose?labels[size_t(coordinate)*width+line]:labels[size_t(line)*width+coordinate];};
        auto& output=transpose?vertical:horizontal;
        for(int line=0;line<lines;++line){
            currentRuns.clear();
            for(int i=0;i<length;){
                const auto value=pixel(i,line);if(!value){++i;continue;}
                const int begin=i++;while(i<length&&pixel(i,line)==value)++i;
                currentRuns.push_back({begin,i,value});
            }
            size_t a=0,b=0;
            while(a<previousRuns.size()&&b<currentRuns.size()){
                const auto first=previousRuns[a],second=currentRuns[b];
                const int begin=std::max(first.begin,second.begin),end=std::min(first.end,second.end);
                // Maximal equal-label intervals make both overlap endpoints reflex
                // vertices exactly when both starts and both ends differ.
                if(first.label==second.label&&begin<end&&first.begin!=second.begin&&first.end!=second.end)
                    output.push_back({line,begin,end});
                if(first.end<=second.end)++a;
                if(second.end<=first.end)++b;
            }
            previousRuns.swap(currentRuns);
        }
    }
    template<class Emit> void SweepConflicts(int width,int height,Emit emit) {
        activeHorizontal.assign(size_t(height)+1,-1);
        for(int x=0;x<=width;++x){
            // Closed segment intersections: starts, queries, then ends. Sharing a
            // reflex endpoint is a conflict, not just proper interior crossings.
            for(const int h:starts[x])activeHorizontal[horizontal[h].fixed]=h;
            for(const int v:queries[x])for(int y=vertical[v].begin;y<=vertical[v].end;++y)
                if(activeHorizontal[y]>=0)emit(activeHorizontal[y],v);
            for(const int h:ends[x])activeHorizontal[horizontal[h].fixed]=-1;
        }
    }
    void BuildConflictGraph(int width,int height) {
        starts.resize(size_t(width)+1);ends.resize(size_t(width)+1);queries.resize(size_t(width)+1);
        for(auto& row:starts)row.clear();for(auto& row:ends)row.clear();for(auto& row:queries)row.clear();
        for(int h=0;h<int(horizontal.size());++h){starts[horizontal[h].begin].push_back(h);ends[horizontal[h].end].push_back(h);}
        for(int v=0;v<int(vertical.size());++v)queries[vertical[v].fixed].push_back(v);
        offsets.assign(horizontal.size()+1,0);
        SweepConflicts(width,height,[&](int h,int){++offsets[h+1];});
        for(size_t i=1;i<offsets.size();++i)offsets[i]+=offsets[i-1];
        edges.resize(size_t(offsets.back()));writeOffsets=offsets;
        SweepConflicts(width,height,[&](int h,int v){edges[size_t(writeOffsets[h]++)]=v;});
    }
    bool Augment(int root,int shortest) {
        path.clear();path.push_back({root,-1});
        while(!path.empty()){
            const int h=path.back().left;bool descended=false;
            while(cursors[h]<offsets[h+1]){
                const int v=edges[size_t(cursors[h]++)],matched=rightPair[v];
                if(matched<0){
                    if(distance[h]+1!=shortest)continue;
                    leftPair[h]=v;rightPair[v]=h;
                    for(size_t i=path.size()-1;i>0;--i){
                        const int parent=path[i-1].left,right=path[i].viaRight;
                        leftPair[parent]=right;rightPair[right]=parent;
                    }
                    return true;
                }
                if(distance[matched]==distance[h]+1&&distance[matched]<shortest){
                    path.push_back({matched,v});descended=true;break;
                }
            }
            if(!descended){distance[h]=-1;path.pop_back();}
        }
        return false;
    }
    void SelectIndependentChords() {
        leftPair.assign(horizontal.size(),-1);rightPair.assign(vertical.size(),-1);
        // Linear greedy warm start, followed by exact Hopcroft-Karp augmentation.
        for(int h=0;h<int(horizontal.size());++h)for(int e=offsets[h];e<offsets[h+1];++e){
            const int v=edges[size_t(e)];if(rightPair[v]<0){leftPair[h]=v;rightPair[v]=h;break;}
        }
        for(;;){
            distance.assign(horizontal.size(),-1);queue.clear();
            for(int h=0;h<int(horizontal.size());++h)if(leftPair[h]<0){distance[h]=0;queue.push_back(h);}
            int shortest=std::numeric_limits<int>::max();
            for(size_t head=0;head<queue.size();++head){
                const int h=queue[head];if(distance[h]>=shortest)continue;
                for(int e=offsets[h];e<offsets[h+1];++e){
                    const int next=rightPair[edges[size_t(e)]];
                    if(next<0)shortest=distance[h]+1;
                    else if(distance[next]<0){distance[next]=distance[h]+1;queue.push_back(next);}
                }
            }
            if(shortest==std::numeric_limits<int>::max())break;
            cursors=offsets;
            for(int h=0;h<int(horizontal.size());++h)if(leftPair[h]<0)Augment(h,shortest);
        }
        // Alternating reachability from unmatched H gives the independent set
        // Z_H union (V minus Z_V), the complement of a minimum vertex cover.
        seenLeft.assign(horizontal.size(),0);seenRight.assign(vertical.size(),0);queue.clear();
        for(int h=0;h<int(horizontal.size());++h)if(leftPair[h]<0){seenLeft[h]=1;queue.push_back(h);}
        for(size_t head=0;head<queue.size();++head){
            const int h=queue[head];
            for(int e=offsets[h];e<offsets[h+1];++e){
                const int v=edges[size_t(e)];if(leftPair[h]==v||seenRight[v])continue;
                seenRight[v]=1;const int next=rightPair[v];
                if(next>=0&&!seenLeft[next]){seenLeft[next]=1;queue.push_back(next);}
            }
        }
    }
public:
    std::vector<SurfaceRectangle> Decompose(const std::vector<uint32_t>& labels,int width,int height) {
        if(width<1||height<1||width>256||height>256||labels.size()!=size_t(width)*height)
            throw std::invalid_argument("VOX rectangle decomposition requires a valid plane up to 256x256");
        horizontal.clear();vertical.clear();
        ExtractChords(labels,width,height,false);ExtractChords(labels,width,height,true);
        BuildConflictGraph(width,height);SelectIndependentChords();
        horizontalCuts.assign(size_t(height+1)*width,0);verticalCuts.assign(size_t(height)*(width+1),0);
        for(size_t h=0;h<horizontal.size();++h)if(seenLeft[h]){
            const auto cut=horizontal[h];for(int x=cut.begin;x<cut.end;++x)horizontalCuts[size_t(cut.fixed)*width+x]=1;
        }
        for(size_t v=0;v<vertical.size();++v)if(!seenRight[v]){
            const auto cut=vertical[v];for(int y=cut.begin;y<cut.end;++y)verticalCuts[size_t(y)*(width+1)+cut.fixed]=1;
        }
        std::vector<SurfaceRectangle> result;active.clear();
        for(int y=0;y<height;++y){
            rowRuns.clear();
            for(int x=0;x<width;){
                const auto label=labels[size_t(y)*width+x];if(!label){++x;continue;}
                const int begin=x++;
                while(x<width&&labels[size_t(y)*width+x]==label&&!verticalCuts[size_t(y)*(width+1)+x])++x;
                rowRuns.push_back({begin,y,x-begin,1,label});
            }
            nextActive.clear();size_t a=0,b=0;
            while(a<active.size()&&b<rowRuns.size()){
                const auto previous=active[a],run=rowRuns[b];bool blocked=false;
                if(previous.x==run.x&&previous.width==run.width&&previous.label==run.label)
                    for(int x=run.x;x<run.x+run.width;++x)blocked|=horizontalCuts[size_t(y)*width+x]!=0;
                if(previous.x==run.x&&previous.width==run.width&&previous.label==run.label&&!blocked){
                    auto extended=previous;++extended.height;nextActive.push_back(extended);++a;++b;
                }else if(std::pair{previous.x,previous.label}<=std::pair{run.x,run.label}){
                    result.push_back(previous);++a;
                }else{nextActive.push_back(run);++b;}
            }
            while(a<active.size())result.push_back(active[a++]);
            while(b<rowRuns.size())nextActive.push_back(rowRuns[b++]);
            active.swap(nextActive);
        }
        result.insert(result.end(),active.begin(),active.end());return result;
    }
};
}
