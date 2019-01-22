// amr_report.cpp

/*===========================================================================
*
*                            PUBLIC DOMAIN NOTICE                          
*               National Center for Biotechnology Information
*                                                                          
*  This software/database is a "United States Government Work" under the   
*  terms of the United States Copyright Act.  It was written as part of    
*  the author's official duties as a United States Government employee and 
*  thus cannot be copyrighted.  This software/database is freely available 
*  to the public for use. The National Library of Medicine and the U.S.    
*  Government have not placed any restriction on its use or reproduction.  
*                                                                          
*  Although all reasonable efforts have been taken to ensure the accuracy  
*  and reliability of the software and data, the NLM and the U.S.          
*  Government do not and cannot warrant the performance or results that    
*  may be obtained by using this software or data. The NLM and the U.S.    
*  Government disclaim all warranties, express or implied, including       
*  warranties of performance, merchantability or fitness for any particular
*  purpose.                                                                
*                                                                          
*  Please cite the author in any work or product based on this material.   
*
* ===========================================================================
*
* Author: Vyacheslav Brover
*
* File Description:
*   Identification of AMR genes using BLAST and HMM search results vs. the NCBI virulence database
*
*/
   
   
// PAR ??
//#define VER 1  
//#define VER 2  // PD-2074
#define VER 3  // PD-2109
//
#if VER <= 0
  #error "Negative version " VER
#endif



#undef NDEBUG 
#include "common.inc"

#include "common.hpp"
using namespace Common_sp;
#include "gff.hpp"
using namespace GFF_sp;



static constexpr bool useCrossOrigin = false;  // GPipe: true
static constexpr char pm_delimiter = '_';



namespace 
{


constexpr static double frac_delta = 1e-5;  // PAR      
const double NaN = numeric_limits<double>::quiet_NaN ();  



struct BlastRule
{
  // 0 <=> undefined
  // 0 .. 1
  double ident {0.0};
  double target_coverage {0.0};
  double ref_coverage {0.0};

  BlastRule (double ident_arg,
             double target_coverage_arg,
             double ref_coverage_arg)
    : ident           (ident_arg)
    , target_coverage (target_coverage_arg)
    , ref_coverage    (ref_coverage_arg)
    {
      ASSERT (ident >= 0.0);
      ASSERT (ident <= 1.0);
      ASSERT (target_coverage >= 0.0);
      ASSERT (target_coverage <= 1.0);
      ASSERT (ref_coverage >= 0.0);
      ASSERT (ref_coverage <= 1.0);
    //ASSERT ((bool) ident == (bool) target_coverage);
    //ASSERT ((bool) ref_coverage == (bool) target_coverage);
    }  
  BlastRule () = default;
    
  bool empty () const
    { return ! ident; }
};

BlastRule globalCompleteBR;
BlastRule globalPartialBR;



struct Batch;  // forward



struct Fam 
// Table PROTEUS.VF..FAM
{
	const Fam* parent {nullptr};
	  // Tree
  string id; 
  string genesymbol;
  string familyName; 
  bool reportable {false}; 

  // HMM
  string hmm; 
    // May be empty()
  double tc1 {NaN}; 
  double tc2 {NaN}; 


  Fam (Fam* parent_arg,
       const string &id_arg,
       const string &genesymbol_arg,
       const string &hmm_arg,
       double tc1_arg,
       double tc2_arg,
       const string &familyName_arg,
       bool reportable_arg)
    : parent (parent_arg)
    , id (id_arg)
    , genesymbol (genesymbol_arg)
    , familyName (familyName_arg)
    , reportable (reportable_arg)
    , hmm (hmm_arg)
    , tc1 (tc1_arg)
    , tc2 (tc2_arg)
    { if (genesymbol == "-")
        genesymbol. clear ();
      if (hmm == "-")
        hmm. clear ();
      if (hmm. empty () != ! tc1)
      { cout << id << ' ' << hmm << ' ' << tc1 << endl;
        ERROR;
      }
      ASSERT (hmm. empty () == ! tc2); 
    //IMPLY (! hmm. empty (), tc2 > 0);
      if (familyName == "NULL")
        familyName. clear ();
      ASSERT (tc2 >= 0);        
      ASSERT (tc2 <= tc1);
    }
  Fam () = default;
  void saveText (ostream &os) const 
    { os << hmm << " " << tc1 << " " << tc2 << " " << familyName << " " << reportable; }


	bool descendantOf (const Fam* ancestor) const
	  { if (! ancestor)
	  	  return true;
	  	if (this == ancestor)
	  		return true;
	  	if (parent)
	  		return parent->descendantOf (ancestor);
	  	return false;
	  }
  const Fam* getHmmFam () const
    // Return: most specific HMM
    { const Fam* f = this;
      while (f && f->hmm. empty ())
        f = static_cast <const Fam*> (f->parent);
      return f;
    }
};


map<string/*famId*/,const Fam*> famId2fam;



struct BlastAlignment;



struct HmmAlignment 
// Query: AMR HMM
{
  string sseqid; 
  double score1 {NaN}; 
  double score2 {NaN}; 
    // May be different from max(Domain::score)
  const Fam* fam {nullptr};
    // Query
//ali_from, ali_to ??
  unique_ptr<const BlastAlignment> blastAl;
    // (bool)get()
  
  
  HmmAlignment (const string &line,
                const Batch &batch);
    // Update: batch.domains
  HmmAlignment (const HmmAlignment &other);
  HmmAlignment operator= (const HmmAlignment &other);
  HmmAlignment () = default;
  void saveText (ostream &os) const 
    { os << sseqid << ' ' << score1 << ' ' << score2 << ' ' << (fam ? fam->hmm : string ()); }
      
      
  bool good () const
    { /*cout << fam << endl;
    	if (fam)
    		cout << fam->id << ' ' << fam->hmm << ' ' << fam->tc1 << ' ' << fam->tc2 << endl; */
    	return    fam 
             && ! fam->hmm. empty ()
             && score1 >= fam->tc1
             && score2 >= fam->tc2
           //&& fam->reportable
             ; 
    }
private:
  bool betterEq (const HmmAlignment &other,
                 unsigned char criterion) const
    // Reflexive
    // For one sseqid: one HmmAlignment is better than all others
    { ASSERT (good ());
      ASSERT (other. fam);
      if (sseqid != other. sseqid)
        return false;
      switch (criterion)
      {
        case 0: return fam->descendantOf (other. fam); 
        case 1: 
          {
            LESS_PART (other, *this, score1);
          //return score2 >= other. score2;  // GP-16770
            LESS_PART (other, *this, fam->tc1);
            LESS_PART (*this, other, fam->id);  // Tie resolution
            return true;
          }
        default: ERROR;
      }
      throw logic_error ("Never call");
    }
public:
  bool better (const HmmAlignment &other,
               unsigned char criterion) const
    { return betterEq (other, criterion) && ! other. betterEq (*this, criterion); }


  typedef  pair<string/*sseqid*/,string/*FAM.id*/>  Pair;
  
  
  struct Domain  
  {
    double score {0};
    size_t hmmLen {0};
    size_t hmmStart {0}; 
    size_t hmmStop {0};
    size_t seqLen {0}; 
    size_t seqStart {0};
    size_t seqStop {0}; 
    

    Domain (const string &line,
            Batch &batch);
      // Input: line: hmmsearch -domtable line
    Domain () = default;
  };
};



struct PointMut
{
	size_t pos {0};
	  // In reference sequence
	  // >= 0
	char alleleChar {' '};
	  // Upper-case
	// !empty()
	string geneMutation;
	  // Depends on the above
	string geneMutationGen;
	  // geneMutation generalized, may have a different pos
	string name;
	  // Species binomial + resistance
	bool additional {false};

	
	PointMut (size_t pos_arg,
						char alleleChar_arg,
						const string &geneMutation_arg,
						const string &geneMutationGen_arg,
						const string &name_arg)
		: pos (pos_arg)
		, alleleChar (toUpper (alleleChar_arg))
		, geneMutation (geneMutation_arg)
		, geneMutationGen (geneMutationGen_arg)
		, name (name_arg)
		{ ASSERT (pos > 0);
			pos--;
			ASSERT (alleleChar != ' ');
			ASSERT (! geneMutation. empty ());
			ASSERT (! geneMutationGen. empty ());
			if (isRight (geneMutation, "STOP"))
				ASSERT (alleleChar == '*')
			else
        ASSERT (geneMutation. back () == alleleChar);
      ASSERT (geneMutation. front () != alleleChar);
			if (isRight (geneMutationGen, "STOP"))
				ASSERT (alleleChar == '*')
			else
        ASSERT (geneMutationGen. back () == alleleChar);
      ASSERT (geneMutationGen. front () != alleleChar);
			ASSERT (! name. empty ());
      ASSERT (! contains (name, '\t'));
      replace (name, '_', ' ');
      ASSERT (! contains (name, "  "));
		}
	PointMut (const string &gene,
	          size_t pos_arg,
	          char refChar,
	          char alleleChar_arg)
	  : pos (pos_arg)
	  , alleleChar (alleleChar_arg)
	  , geneMutation (gene + pm_delimiter + refChar + toString (pos + 1) + alleleChar)
	  , geneMutationGen (geneMutation)
	  , name (string (refChar == alleleChar ? "wildtype" : "mutation") + " " + strUpper1 (gene))
	  , additional (true)
	  { ASSERT (! gene. empty ());
	  	ASSERT (isUpper (refChar));
	  	ASSERT (isUpper (alleleChar));
	  	ASSERT (refChar != ' ');
	  	ASSERT (alleleChar != ' ');
	  }
	PointMut () = default;


  bool empty () const
    { return name. empty (); }
  string getResistance () const
    { size_t p = name. find (' ');
    	ASSERT (p != string::npos);
    	p = name. find (' ', p + 1);
    	ASSERT (p != string::npos);
    	return name. substr (p + 1);
    }
  void print (ostream &os) const
    { os << pos << ' ' << alleleChar << ' ' << geneMutation << ' ' << geneMutationGen << ' ' << name << endl; }
  bool operator< (const PointMut &other) const
    { LESS_PART (*this, other, pos);
      LESS_PART (*this, other, alleleChar);
      return false;
    }
  bool operator== (const PointMut &other) const
    { return    pos        == other. pos
    	       && alleleChar == other. alleleChar;
    }
};


map <string/*accession*/, Vector<PointMut>>  accession2pointMuts;


double complete_cover_min = NaN;

//bool combiner = false;
bool cdsExist = false;
bool print_fam = false;

bool reportPseudo = false; 
const string stopCodonS ("[stop]");
const string frameShiftS ("[frameshift]");

unique_ptr<OFStream> point_mut_all;



struct BlastAlignment 
{
  // BLAST alignment
  size_t length {0}, nident {0}  // aa
       ,    refStart {0},    refStop {0},    refLen {0}
       , targetStart {0}, targetStop {0}, targetLen {0};
    // Positions are 0-based
    // start < stop

  // target    
  string targetName; 
  bool targetProt {true};
    // false <=> DNA
  bool targetStrand {true}; 
    // false <=> negative
  size_t targetAlign {0};
  size_t targetAlign_aa {0};
  bool partialDna {false};
  bool stopCodon {false}; 
  bool frameShift {false};
  
  // Reference (AMR) protein
  long gi {0};  
    // 0 <=> HMM method
  string accessionProt; 
  size_t part {1};    
    // >= 1
    // <= parts
  size_t parts {1};  
    // >= 1
  // Table FAM
  string famId;  
  string gene;   
    // FAM.class  
  string resistance;

  BlastRule completeBR;
  BlastRule partialBR;
  
  string product;  

  Vector<Locus> cdss;
  Vector<PointMut> pointMuts;
  
  static constexpr size_t mismatchTail_aa = 10;  // PAR
  size_t mismatchTailTarget {0};


  BlastAlignment (const string &line,
                  bool targetProt_arg)
    : targetProt (targetProt_arg)
    {
    	try 
    	{
	      string refName, targetSeq, refSeq;
	      static Istringstream iss;
		    iss. reset (line);
		    iss >> targetName >> refName >> length >> nident >> targetStart >> targetStop >> targetLen >> refStart >> refStop >> refLen >> targetSeq >> refSeq;
		  // format:  qseqid      sseqid    length    nident         qstart         qend         qlen      sstart      send      slen         sseq    qseq
	    // blastp:  ...         ...          663       169              2          600          639           9       665       693          ...
	    // blastx:  ...         ...          381       381          13407        14549        57298           1       381       381          ...
		    ASSERT (! targetSeq. empty ());	
		    ASSERT (targetSeq. size () == refSeq. size ());    

        try
        {	
		      // refName	    
			    product                     =                     rfindSplit (refName, '|'); 
			  #if VER >= 3			    
          partialBR.  ref_coverage    =       str2<double> (rfindSplit (refName, '|')); 
          partialBR.  target_coverage =       str2<double> (rfindSplit (refName, '|')); 
          partialBR.  ident           =       str2<double> (rfindSplit (refName, '|')); 
          completeBR. ref_coverage    =       str2<double> (rfindSplit (refName, '|')); 
          completeBR. target_coverage =       str2<double> (rfindSplit (refName, '|')); 
          completeBR. ident           =       str2<double> (rfindSplit (refName, '|')); 
        #endif
			    resistance                  =                     rfindSplit (refName, '|'); 
			    gene                        =                     rfindSplit (refName, '|');  // Reportable_vw.class
			    famId                       =                     rfindSplit (refName, '|');  // Reportable_vw.fam
			    parts                       = (size_t) str2<int> (rfindSplit (refName, '|'));
			    part                        = (size_t) str2<int> (rfindSplit (refName, '|'));
			  #if VER == 1
			                                                      rfindSplit (refName, '|');  // nucl_accession
			  #endif
			    accessionProt               =                     rfindSplit (refName, '|');
			    gi = str2<long> (refName);
			  }
			  catch (const exception &e)
			  {
			  	throw runtime_error (string ("Bad AMRFinder database\n") + e. what ());
			  }
		    ASSERT (gi > 0);
		    	    
		    replace (product, '_', ' ');
		    
		    toProb (completeBR. ident);
		    toProb (completeBR. target_coverage);
		    toProb (completeBR. ref_coverage);
		    toProb (partialBR.  ident);
		    toProb (partialBR.  target_coverage);
		    toProb (partialBR.  ref_coverage);
		    
		    if (completeBR. empty ())
		      completeBR = globalCompleteBR;
		    if (partialBR. empty ())
		      partialBR = globalPartialBR;
		    
		    ASSERT (refStart < refStop);  
	
		    ASSERT (targetStart != targetStop);
		    targetStrand = targetStart < targetStop;  
		    IMPLY (targetProt, targetStrand);
		    if (! targetStrand)
		      swap (targetStart, targetStop);
		      
		    ASSERT (refStart >= 1);
		    ASSERT (targetStart >= 1);
		    ASSERT (refStart < refStop);
		    ASSERT (targetStart < targetStop);
		    refStart--;
		    targetStart--;
	
		    partialDna = false;
		    constexpr size_t mismatchTailDna = 10;  // PAR
		    if (! targetProt && targetStop - targetStart >= 30)  // PAR, PD-671
		    {
		           if (refStart > 0      && targetTail (true)  <= mismatchTailDna)  partialDna = true;
		      else if (refStop  < refLen && targetTail (false) <= mismatchTailDna)  partialDna = true;
		    }
	
	      setTargetAlign ();	    
		    
		    if (contains (targetSeq, "*"))  
		      stopCodon  =  true;
		  //frameShift = contains (targetSeq, "/");  // Needs "blastall -p blastx ... "
		    if (! targetProt && (targetStop - targetStart) % 3 != 0)
	  	    frameShift = true;	  
		  	  
		    // For BLASTX
		  	// PD-1280
		    if (   ! targetProt 
		        && refStart == 0 
		        && charInSet (targetSeq [0], "LIV") 
		        && nident < targetAlign_aa
		       )
		      nident++;
		    	    
		    mismatchTailTarget = mismatchTail_aa;
		    if (! targetProt)
		      mismatchTailTarget *= 3;
	
	      strUpper (targetSeq);
	      strUpper (refSeq);
		    if (const Vector<PointMut>* pointMuts_all = findPtr (accession2pointMuts, accessionProt))
		    {
		    	if (verbose ())
		        cout << "PointMut protein found: " << refName << endl;
  	      ASSERT (isPointMut ());
		      ASSERT (! pointMuts_all->empty ());
		      string s (pointMuts_all->front (). geneMutation);
		      rfindSplit (s, pm_delimiter);  
		      const string pmGene (s);
	    		size_t refPos = refStart;
	    		size_t j = 0;
	  		  while (j < pointMuts_all->size () && pointMuts_all->at (j). pos < refPos)
	  		  {
	  		  	if (point_mut_all. get ())
	  		  	{
	  		  		PointMut pm (pointMuts_all->at (j));
	  		  		pm. name = "Non-called " + pmGene;
	  		  		pm. additional = true;
	  		  		pointMuts << move (pm);
	  		  	}
	  		    j++;
	  		  }
	   		  VectorPtr<PointMut> pos_pm;  // PointMut's of the same refPos
	    		FFOR (size_t, i, refSeq. size ())
	    	  {
	    		  if (refSeq [i] == '-')
	    		  	continue;
	    		  pos_pm. clear ();
	    		  while (j < pointMuts_all->size () && pointMuts_all->at (j). pos == refPos)
	    		  {
	    		  	pos_pm << & pointMuts_all->at (j);
	    		    j++;
	    		  }
			      if (targetSeq [i] == refSeq [i])
			      	if (pos_pm. empty ())
			      		;
			      	else
			      	{
				      	for (const PointMut* pm : pos_pm)
					    		if (targetSeq [i] == pm->alleleChar)
					    			throw logic_error ("Wildtype allele " + pm->geneMutation);
					    	if (point_mut_all. get ())
					    		pointMuts << move (PointMut (pmGene, refPos, refSeq [i], targetSeq [i]));
			      	}
			      else
			      {
			      //bool found = false;
			      	for (const PointMut* pm : pos_pm)
				    		if (targetSeq [i] == pm->alleleChar)
				    		{
			    			  pointMuts << *pm;
				    		//found = true;
				    		}
				    #if 0
				    	if (! found && point_mut_all. get ())
				    		pointMuts << move (PointMut (pmGene, refPos, refSeq [i], targetSeq [i]));
				    #endif
				    }
	  		  	refPos++;
	    		}
	 		  	if (point_mut_all. get ())
		  		  while (j < pointMuts_all->size ())
		  		  {
				  		PointMut pm (pointMuts_all->at (j));
				  		pm. name = "Non-called " + pmGene;
				  		pm. additional = true;
				  		pointMuts << move (pm);
		  		    j++;
		  		  }
		    }
		  }
		  catch (...)
		  {
		  	cout << line << endl;
		  	throw;
		  }
    }
private:
  static void toProb (double &x)
    { ASSERT (x >= 0.0);
      ASSERT (x <= 100.0);
      x /= 100.0;
    }
public:
  explicit BlastAlignment (const HmmAlignment& other)
    : targetName (other. sseqid)     
    , famId      (other. fam->id)   
    , gene       (other. fam->id)   
    , product    (other. fam->familyName)   
    { if (allele ())
        ERROR_MSG (famId + " " + gene);
    }
  void qc () const
    {
      if (! qc_on)
        return;
	    ASSERT (! famId. empty ());
	    ASSERT (! gene. empty ());
	    ASSERT (part >= 1);
	    ASSERT (part <= parts);
	    ASSERT (! product. empty ());
	    IMPLY (targetProt, targetStrand);  
	    ASSERT (targetStart < targetStop);
	    ASSERT (targetStop <= targetLen);
	    ASSERT ((bool) gi == (bool) length);
	    ASSERT ((bool) gi == (bool) refLen);
	    ASSERT ((bool) gi == (bool) nident);
	    ASSERT ((bool) gi == ! accessionProt. empty ());
	    IMPLY (! gi && ! isPointMut (), getFam () -> getHmmFam ());
	    IMPLY (targetProt, ! partialDna);
	  //IMPLY (! targetProt, (targetStop - targetStart) % 3 == 0);
	    ASSERT (targetAlign);
	    IMPLY (targetProt, targetAlign == targetAlign_aa);
	    IMPLY (! targetProt, targetAlign == 3 * targetAlign_aa);
	    ASSERT (nident <= targetAlign_aa);
	    IMPLY (! targetProt, cdss. empty ());
	    if (gi)
	    {
	      ASSERT (refStart < refStop);
  	    ASSERT (nident <= refStop - refStart);
  	    ASSERT (refStop <= refLen);
  	    ASSERT (refStop - refStart <= length);	    
  	    ASSERT (targetAlign_aa <= length);
	    }
	  #if 0
	    if (targetProt)
    	  for (const Locus& cds : cdss)
    	    ASSERT (   cds. size () == 3 * targetLen + 3
    	            || cds. size () == 3 * targetLen 
    	           );
    #endif
	    IMPLY (! pointMuts. empty (), isPointMut ());
    }
  void saveText (ostream& os) const 
    { // PD-736, PD-774, PD-780, PD-799
      const string na ("NA");
      const string proteinName (isPointMut () 
                                  ? string ()
                                  : refExactlyMatched () || ! gi 
                                    ? product 
                                    : nvl (getFam () -> familyName, na)
                               );
      ASSERT (! contains (proteinName, '\t'));
      Vector<Locus> cdss_ (cdss);
      if (cdss_. empty ())
        cdss_ << Locus ();
      Vector<PointMut> pointMuts_ (pointMuts);
      if (pointMuts_. empty ())
        pointMuts_ << PointMut ();
      for (const Locus& cds : cdss_)
	      for (const PointMut& pm : pointMuts_)
	      {
          const string method (getMethod (cds));
	        TabDel td (2, false);
          td << targetName;
	        if (cdsExist)
	          td << (cds. contig. empty () ? targetName : cds. contig)
	             << (cds. contig. empty () ? targetStart : cds. start) + 1
	             << (cds. contig. empty () ? targetStop  : cds. stop)
	             << (cds. contig. empty () ? (targetStrand ? '+' : '-') : (cds. strand ? '+' : '-'));
	        td << (pm. empty ()
	                 ? print_fam 
			                 ? famId
			                 : (isLeft (method, "ALLELE") ? famId : nvl (getFam () -> genesymbol, na))
			             : pm. geneMutation
	              )
	           <<   (pm. empty () ? proteinName : pm. name)
	              + ifS (reportPseudo, ifS (frameShift, " " + frameShiftS))
	           << method
	           << (targetProt ? targetLen : targetAlign_aa);  
	        if (gi)
	          td << refLen
	             << refCoverage () * 100  
	             << pIdentity () * 100  // refIdentity
	             << length
	             << accessionProt
	             << product
	             ;
	        else
	          td << na 
	             << na
	             << na
	             << na
	             << na
	             << na;
	        // PD-775
	        if (isPointMut ())
	          td << na
	             << na;
	        else if (const Fam* f = getFam () -> getHmmFam ())
	          td << f->hmm
	             << f->familyName;
	        else
	        {
	          td << na
	             << na;
	          ASSERT (method != "HMM");
	        }
	        if (cdsExist)
	        {
	        	IMPLY (cds. crossOrigin, useCrossOrigin);
	        	if (useCrossOrigin)
	        	{
		          if (cds. crossOrigin) 
		            td << cds. contigLen;
		          else 
		            td << na;        	
		        }
	        }
	        if (pm. empty () || ! pm. additional)
	          os << td. str () << endl;
	        if (point_mut_all. get () && ! pm. empty ())
	          *point_mut_all << td. str () << endl;
	      }
    }
    

  bool isPointMut () const
    { return resistance == "point_mutation"; }
  bool allele () const
    { return famId != gene && parts == 1; }
  size_t targetTail (bool upstream) const
    { return targetStrand == upstream ? targetStart : (targetLen - targetStop); }
  size_t refEffectiveLen () const
    { return partialDna ? refStop - refStart : refLen; }
  double pIdentity () const
    { return (double) nident / (double) length; }
  double refCoverage () const
    { return (double) (refStop - refStart) / (double) refLen; }
  double targetCoverage () const
    { return targetProt ? (double) (targetStop - targetStart) / (double) targetLen : NaN; }
  bool refExactlyMatched () const
    { return    refLen   
             && nident == refLen 
             && refLen == length;
	  }
  bool targetExactlyMatched () const
    { return    targetLen   
             && nident == targetLen 
             && targetLen == length;
	  }
  bool partial () const
    // Requires: good()
    { return refCoverage () < complete_cover_min - frac_delta; }
  bool getTargetStrand (const Locus &cds) const
    { return targetProt
               ? cds. empty ()
                 ? true
                 : cds. strand
               : targetStrand;
    }
  size_t missedDnaStart (const Locus &cds) const
    { return 3 * (getTargetStrand (cds)
                    ? refStart
                    : refLen - refStop
                 );
    }
  size_t missedDnaStop (const Locus &cds) const
    { return 3 * (getTargetStrand (cds)
                    ? refLen - refStop
                    : refStart
                 );
    }
  bool truncated (const Locus &cds) const
    { return    (missedDnaStart (cds) > 0 && (targetProt ? (cds. empty () ? false : cds. atContigStart ()) : targetStart            <= Locus::end_delta))
             || (missedDnaStop  (cds) > 0 && (targetProt ? (cds. empty () ? false : cds. atContigStop  ()) : targetLen - targetStop <= Locus::end_delta));
    }
  bool alleleReported () const
    { return refExactlyMatched () && allele () && (! targetProt || refLen == targetLen); }
	string getMethod (const Locus &cds) const
	  { string method (refExactlyMatched () 
        	             ? alleleReported () 
        	               ? "ALLELE"
        	               : "EXACT"  // PD-776
        	             : gi
        	                ? partial ()
        	                  ? truncated (cds)
        	                    ? "PARTIAL_CONTIG_END"  // PD-2267
        	                    : "PARTIAL"
        	                  : isPointMut ()
        	                    ? "POINT"
        	                    : "BLAST"
        	                : "HMM"
        	           );
      // PD-2088, PD-2320
	    if (   (   method == "BLAST" 
    	        || method == "PARTIAL"
    	        || method == "PARTIAL_CONTIG_END"
    	       ) 
	        && stopCodon
	        && ! targetProt
	       )
	      method = "INTERNAL_STOP";	
	    else if (method != "HMM")
	      method += (targetProt ? "P" : "X");	  
	    return method;
	  }
	  // PD-736
	bool passBlastRule (const BlastRule &br) const
	  { return    pIdentity ()      >= br. ident        - frac_delta
  	         && refCoverage ()    >= br. ref_coverage - frac_delta
  	       /*&& (   ! targetProt 
  	             || targetCoverage () >= br. target_coverage - frac_delta
  	            )*/;  // PD-2124
	  }
  bool good () const
    { if (! reportPseudo)
      {
        if (stopCodon)
          return false; 
        if (frameShift)
          return false;
        if (partial () && ! cdss. empty ())
        {
          bool found = false;
          for (const Locus& cds : cdss)
            if (truncated (cds))
              found = true;
          if (! found)
            return false;
        }
      }
  	  // PD-1032
	    if (partial ())
  	    if (parts > 1)
  	    	return false;
	      else
	        return passBlastRule (partialBR);
	    else
	      return passBlastRule (completeBR);
    }
private:
  bool insideEq (const BlastAlignment &other) const
    { ASSERT (targetProt == other. targetProt);
    	return    targetStrand                     == other. targetStrand
             && targetStart + mismatchTailTarget >= other. targetStart 
             && targetStop                       <= other. targetStop + mismatchTailTarget;	    
    }
    // Requires: same targetName
#if 0
  bool descendantOf (const BlastAlignment &other) const
    { return    ! other. allele ()
             && getFam () -> descendantOf (other. getFam ());
    }
#endif
  bool matchesCds (const BlastAlignment &other) const
    { ASSERT (targetProt);
    	ASSERT (! other. targetProt);
    	ASSERT (! cdss. empty ());
    	for (const Locus& cds : cdss)
    		if (   cds. contig == other. targetName
    			  && cds. strand == other. targetStrand
    			  && ! cds. crossOrigin
    			 )
    		{ size_t protStart = cds. start;
    			size_t protStop  = cds. stop;
    			if (cds. strand)
    			{
    				ASSERT (protStop > 3);
    				protStop -= 3;
    			}
    			else
    			  protStart += 3;
    			ASSERT (protStart < protStop);
    			const size_t dnaStart  = other. targetStart;
    			const size_t dnaStop   = other. targetStop;
    			ASSERT (dnaStart < dnaStop);
    		#if 0
    		  // PD-2271: partial 
    		  // use truncated(cds) ??
    			if (cds. strand)  // Checking frames
    			{ if (difference (protStart, dnaStart) > 3 && protStart % 3 != dnaStart % 3)
    			    continue;
    			}
    			else
    			  if (difference (protStop, dnaStop) > 3 && protStop % 3 != dnaStop % 3)
    			    continue;
    		#endif
    		#if 0  // PD-2099
    			if (cds. strand)  // PD-1902
    			{ if (protStop != dnaStop)
    				  continue;
    			}
    		  else
    				if (protStart != dnaStart)
    				  continue;
    		#endif
    			const size_t intersectionStart = max (protStart, dnaStart);
    			const size_t intersectionStop  = min (protStop,  dnaStop);
    			if (   intersectionStart < intersectionStop
    				  && double (intersectionStop - intersectionStart) / double (protStop - protStart) > 0.75  // PAR
    				 )
    				return true;
    		}
    	return false;
    }
  bool betterEq (const BlastAlignment &other) const
    // Reflexive
    { if (targetProt == other. targetProt)  
      {
	    	if (targetName != other. targetName)
	        return false;
	      // PD-807
	    //if (! targetProt && ! other. insideEq (*this))
	      if (   ! other. insideEq (*this)
	      	  && !        insideEq (other)
	      	 )
	        return false;
	      LESS_PART (other, *this, refExactlyMatched ());  // PD-1261, PD-1678
	      LESS_PART (other, *this, nident);
	      LESS_PART (*this, other, refEffectiveLen ());
	    }
	    else
	    { // PD-1902, PD-2139, PD-2313, PD-2320
	    	if (targetProt && ! matchesCds (other))
	    	  return false;
	    	if (! targetProt && ! other. matchesCds (*this))
	    	  return false;
	      LESS_PART (other, *this, refExactlyMatched ());  
	      LESS_PART (other, *this, allele ());  
	      LESS_PART (other, *this, alleleReported ());  
	      LESS_PART (other, *this, targetProt);
      /*
	    	if (targetProt)
	    		return matchesCds (other) && ! other. refExactlyMatched ();
    		return    other. matchesCds (*this) 
    		       && refExactlyMatched () 
    		       && allele () 
    		       && ! (other. refExactlyMatched () && other. refLen == other. targetLen);
      */
	    }
      return true;
    }
public:
  const Fam* getFam () const
    { ASSERT (! isPointMut ());
      const Fam* fam = famId2fam [famId];
      if (! fam)
        fam = famId2fam [gene];
      if (! fam)
      	throw runtime_error ("Cannot find hierarchy for " + famId + " / " + gene);
      return fam;
    }
  bool better (const BlastAlignment &other) const
    { return    betterEq (other) 
    	       && (   ! other. betterEq (*this) 
    	           || accessionProt < other. accessionProt  // Tie resolution; PD-1245
    	          );
    }
  bool better (const HmmAlignment& other) const
    { ASSERT (other. good ());
    	ASSERT (other. blastAl. get ());
    	if (isPointMut ())
    	  return false;
    	if (targetProt)
    	{ if (targetName != other. sseqid)
	        return false;
	    }
	    else
	    	if (! other. blastAl->matchesCds (*this))
	    		return false;
      return    refExactlyMatched () 
           //|| getFam () -> getHmmFam () == other. fam
             || getFam () -> descendantOf (other. fam)
             ;
    }
  bool operator< (const BlastAlignment &other) const
    { 
      LESS_PART (*this, other, targetName);
      LESS_PART (*this, other, targetStart);
      LESS_PART (*this, other, famId);
      LESS_PART (*this, other, accessionProt);
      return false;
    }
//size_t lengthScore () const
  //{ return refLen - (refStop - refStart); } 
  void setTargetAlign ()
    { targetAlign = targetStop - targetStart;
      targetAlign_aa = targetAlign;
      if (! targetProt)
      {
        ASSERT (targetAlign % 3 == 0);
        targetAlign_aa = targetAlign / 3;
      }
    }
  void setCdss (const map<string/*seqid*/,string/*locusTag*/> &seqId2locusTag,
                const Annot &annot)
    { ASSERT (targetProt);
    	ASSERT (cdss. empty ());
  	  string locusTag = targetName;
  	  if (! seqId2locusTag. empty ())
  	  {
  	  	string s;
  	  	EXEC_ASSERT (find (seqId2locusTag, locusTag, s));
  	  	locusTag = s;
  	  }
  	  if (const Set<Locus>* cdss_ = findPtr (annot. prot2cdss, locusTag))
  	  {
    	  insertAll (cdss, *cdss_);
    	#if 0
    	  // PD-2269
    	  for (const Locus& cds : *cdss_)
    	    // PD-2138
    	    if (   ! cds. partial
    	        && cds. size () != 3 * targetLen + 3
    	        && cds. size () != 3 * targetLen     // PD-2159
    	       )
    	      throw runtime_error ("Mismatch in protein length between the protein " + targetName 
    	                           + " and the length of the protein on line " + toString (cds. lineNum) 
    	                           + " of the GFF file. Please check that the GFF and protein files match.");
    	#endif
    	}
  	  else
  	    throw runtime_error ("Locus tag " + strQuote (locusTag) + " is misssing in .gff-file. Protein name: " + targetName);
  	  qc ();
    }
};




struct Batch
{
  // Reference input
  map<string/*hmm*/,const Fam*> hmm2fam;
  unique_ptr<Fam> root;
    // Not delete'd in ~Batch()
  bool non_reportable {false};

  // Target input
  typedef  List<BlastAlignment>  BlastAls; 
  typedef  List<HmmAlignment>  HmmAls;   

  BlastAls blastAls;   
  bool hmmExist {false};
  map<HmmAlignment::Pair, HmmAlignment::Domain> domains;  // Best domain  
  HmmAls hmmAls;  
  
  // Output
  BlastAls goodBlastAls; 
  
  
  Batch (const string &famFName,
         const string &organism, 
         const string &point_mut,
         bool non_reportable_arg)
    : root (new Fam ())
    , non_reportable (non_reportable_arg)
	  {
	    if (famFName. empty ())
	    	throw runtime_error ("fam (protein family hierarchy) file is missing");
	    	
	  	// Tree of Fam
	  	// Pass 1  
	  	const string pointMutParent ("POINT_MUTATION");
	    {
	    	if (verbose ())
	    		cout << "Reading " << famFName << " Pass 1 ..." << endl;
	      LineInput f (famFName);  
	  	  while (f. nextLine ())
	  	  {
	  	    trim (f. line);
	      //cout << f. line << endl; 
	  	    const string famId               (findSplit (f. line, '\t'));
	  	    const string parentFamId         (findSplit (f. line, '\t'));
	  	    const string genesymbol          (findSplit (f. line, '\t'));
	  	    const string hmm                 (findSplit (f. line, '\t'));
	  	    const double tc1 = str2<double>  (findSplit (f. line, '\t'));
	  	    const double tc2 = str2<double>  (findSplit (f. line, '\t'));
	  	    const int reportable = str2<int> (findSplit (f. line, '\t'));
	  	    ASSERT (   reportable == 0 
	  	            || reportable == 1
	  	           );
	  	    if (parentFamId == pointMutParent)
	  	      continue;
	  	    const auto fam = new Fam (root. get (), famId, genesymbol, hmm, tc1, tc2, f. line, reportable);
	  	    if (famId2fam [famId])
	  	      throw runtime_error ("Family " + famId + " is duplicated");
	  	    famId2fam [famId] = fam;
	  	    if (! fam->hmm. empty ())
	  	      hmm2fam [fam->hmm] = fam;
	  	  }
	  	}
	  	// Pass 2
	    {
	    	if (verbose ())
	    		cout << "Reading " << famFName << " Pass 2 ..." << endl;
	      LineInput f (famFName);  
	  	  while (f. nextLine ())
	  	  {
	  	    trim (f. line);
	  	  //cout << f. line << endl;  
	  	    Fam* child = var_cast (famId2fam [findSplit (f. line, '\t')]);
	  	    const string parentFamId (findSplit (f. line, '\t'));
	  	    if (parentFamId == pointMutParent)
	  	      continue;
	  	    ASSERT (child);
	  	    Fam* parent = nullptr;
	  	    if (! parentFamId. empty ())
	  	    { 
	  	    	EXEC_ASSERT (parent = var_cast (famId2fam [parentFamId])); 
	  	    }
	  	    child->parent = parent;
	  	  }
	  	}

	    if (! organism. empty ())
	    {
	    	if (verbose ())
	    		cout << "Reading " << point_mut << endl;
	      LineInput f (point_mut);
	      Istringstream iss;
	  	  while (f. nextLine ())
	  	  {
	  	  	string organism_;
	  	  	string accession;
					int pos;
					char alleleChar;
					string geneMutation;
					string geneMutationGen;
					string name;
    	  	iss. reset (f. line);
	  	  	iss >> organism_ >> accession >> pos >> alleleChar >> geneMutation >> geneMutationGen >> name;
	  	  	ASSERT (pos > 0);
	  	  	replace (organism_, '_', ' ');
	  	  	if (organism_ == organism)
	  	  		accession2pointMuts [accession] << move (PointMut ((size_t) pos, alleleChar, geneMutation, geneMutationGen, name));
	  	  }
	  	  // PD-2008
	  	  if (accession2pointMuts. empty ())
	  	  	throw runtime_error ("No protein point mutations for organism " + strQuote (organism) + " found in the AMRFinder database. Please check the " + strQuote ("organism") + " option.");
	  	  for (auto& it : accession2pointMuts)
	  	  {
	  	  	it. second. sort ();
	  	  //if (! it. second. isUniq ())
	  	  	//throw runtime_error ("Duplicate mutations for " + it. first);
	  	  }
	    }
	  }
	  	  

	void process (bool skip_hmm_check) 
  // Input: root, blastAls, domains, hmmAls
	// Output: goodBlastAls
	{
		ASSERT (root. get ());
		
    // Use BlastAlignment.cdss
    for (Iter<BlastAls> iter (blastAls); iter. next ();)
      if (! iter->good ())
        iter. erase ();
        
    // Group by targetName and process each targetName separately for speed ??    

    // BLAST: Pareto-better()  
	  for (const auto& blastAl : blastAls)
    {
    	ASSERT (blastAl. good ());
  	  bool found = false;
  	  for (const auto& goodBlastAl : goodBlastAls)
  	    if (goodBlastAl. better (blastAl))
	      {
	        found = true;
	        break;
	      }
	    if (found)
	      continue;	      
      for (Iter<BlastAls> goodIter (goodBlastAls); goodIter. next ();)
        if (blastAl. better (*goodIter))
          goodIter. erase ();          
      goodBlastAls << blastAl;
    }
  	if (verbose ())
  	{
  	  cout << "# Best Blasts: " << goodBlastAls. size () << endl;
  	  for (const auto& blastAl : goodBlastAls)
  	    blastAl. saveText (cout);
  	}
  #if 0
	  for (const auto& blastAl1 : goodBlastAls)
		  for (const auto& blastAl2 : goodBlastAls)
		  {
		  	cout << endl;
		  	blastAl1. saveText (cout);
		  	blastAl2. saveText (cout);
		  	cout        << blastAl1. better (blastAl2) 
		  	     << ' ' << blastAl2. better (blastAl1) 
		  	     << endl;
		  }
	#endif

  #if 0
    ??
    // Cf. point_mut.cpp
    FFOR (size_t, i, batch. blastAls. size ())
    {
      const BlastAlignment& blastAl1 = batch. blastAls [i];
      for (const auto& it1 : blastAl1. targetPos2pointMut)
      {
        const PointMut& pm1 = it1. second;
        FFOR_START (size_t, j, i + 1, batch. blastAls. size ())
        {
          BlastAlignment& blastAl2 = batch. blastAls [j];
          if (blastAl2. targetName == blastAl1. targetName)  
            for (auto& it2 : blastAl2. targetPos2pointMut)
            {
              PointMut& pm2 = it2. second;
              if (verbose ())
                cout        << blastAl2. targetName 
                     << ' ' << it1. first 
                     << ' ' << it2. first 
                     << ' ' << pm1. geneMutationGen 
                     << ' ' << pm2. geneMutationGen 
                     << endl; 
              if (   it1. first == it2. first
                  && pm1. geneMutationGen == pm2. geneMutationGen
                 )
                pm2 = PointMut ();
            }
        }
      }
    }
  #endif

    // HMM: Pareto-better()  
    HmmAls goodHmmAls; 
    FOR (unsigned char, criterion, 2)
    {
      // hmmAls --> goodHmmAls
      goodHmmAls. clear ();
  	  for (const auto& hmmAl : hmmAls)
      {
      	ASSERT (hmmAl. good ());
    	  bool found = false;
    	  for (const auto& goodHmmAl : goodHmmAls)
    	    if (goodHmmAl. better (hmmAl, criterion))
  	      {
  	        found = true;
  	        break;
  	      }
  	    if (found)
  	      continue;  
        for (Iter<HmmAls> goodIter (goodHmmAls); goodIter. next ();)
          if (hmmAl. better (*goodIter, criterion))
            goodIter. erase ();
        goodHmmAls << hmmAl;
      }
      //
      hmmAls = goodHmmAls;
      if (verbose ())
      {
        cout << "Pareto-better HMMs: (Criterion " << (int) criterion << "): " << hmmAls. size () << endl;
        for (const HmmAlignment& al : hmmAls)
        {
          al. saveText (cout);
          cout << endl;
        }
      }
    }

    // PD-741
  	if (hmmExist && ! skip_hmm_check)
      for (Iter<BlastAls> iter (goodBlastAls); iter. next ();)
        if (   /*! iter->refExactlyMatched () */
               ! iter->isPointMut ()
        	  && iter->targetProt
        	  && iter->pIdentity () < 0.98 - frac_delta  // PAR  // PD-1673
            && ! iter->partial ()
           )
	        if (const Fam* fam = iter->getFam () -> getHmmFam ())  
	        {
	          bool found = false;
	      	  for (const auto& hmmAl : goodHmmAls)
	            if (   iter->targetName == hmmAl. sseqid
	                && fam == hmmAl. fam
	               )
	            {
	              found = true;
	              break;
	            }
	          if (! found)   // BLAST is wrong
	            iter. erase ();
	        }
  	if (verbose ())
  	  cout << "# Best Blasts left: " << goodBlastAls. size () << endl;

    for (Iter<HmmAls> hmmIt (goodHmmAls); hmmIt. next ();)
  	  for (const auto& blastAl : goodBlastAls)
  	    if (blastAl. better (*hmmIt))
	      {
          hmmIt. erase ();
	        break;
	      }

    // Output 
    
    // goodHmmAls --> goodBlastAls
  	for (const auto& hmmAl : goodHmmAls)
  	{
  	  ASSERT (hmmAl. blastAl. get ());
  	  goodBlastAls << * hmmAl. blastAl. get ();
  	}
  
    goodBlastAls. sort ();

    if (verbose ())
    {
	    cout << endl << "After process():" << endl;
		  for (const auto& blastAl : goodBlastAls)
		  {
		    blastAl. saveText (cout);
		    cout << "# Point mutations: " << blastAl. pointMuts. size () << endl;
		  }
		}
	}
	
	
	
	void report (ostream &os) const
	// Input: goodBlastAls
	{
    // PD-283, PD-780
    {
    	// Cf. BlastAlignment::saveText()
	    TabDel td;
	    td << "Target identifier";  // targetName
	    if (cdsExist)  
	      // Contig
	      td << "Contig id"
	         << "Start"  // targetStart
	         << "Stop"  // targetStop
	         << "Strand";  // targetStrand
	    td << (print_fam ? "FAM.id" : "Gene symbol")
	       << "Sequence name"
	       << "Method"
	       << "Target length" 
	       //
	       << "Reference sequence length"         // refLen
	       << "% Coverage of reference sequence"  // queryCoverage
	       << "% Identity to reference sequence"  
	       << "Alignment length"                 // length
	       << "Accession of closest sequence"     // accessionProt
	       << "Name of closest sequence"
	       //
	       << "HMM id"
	       << "HMM description"
	       ;
      if (cdsExist)
	    	if (useCrossOrigin)
	      	 td << "Cross-origin length";
	    os << td. str () << endl;
      if (point_mut_all. get ())
        *point_mut_all << td. str () << endl;
	  }

  	for (const auto& blastAl : goodBlastAls)
  	{
   	  blastAl. qc ();
  	  if (   (non_reportable || (! blastAl. isPointMut () && blastAl. getFam () -> reportable))
  	  	  || ! blastAl. pointMuts. empty ()
  	  	 )
    	  blastAl. saveText (os);
    }
	}



	void printTargetIds (ostream &os) const
	{
		ASSERT (os. good ());
  	for (const auto& blastAl : goodBlastAls)
  	  if (   (non_reportable || (! blastAl. isPointMut () && blastAl. getFam () -> reportable))
  	  	  && blastAl. targetProt
  	  	 )
        os << blastAl. targetName << endl;
	}
};




// HmmAlignment

HmmAlignment::HmmAlignment (const string &line,
                            const Batch &batch)
{
  static Istringstream iss;
  iss. reset (line);
  string hmm, dummy;
  //                                                        --- full sequence ---  --- best 1 domain --
  //     target name  accession  query name     accession   E-value  score     bias     E-value  score  bias   exp reg clu  ov env dom rep inc description of target
  iss >> sseqid >>    dummy      >> dummy      >> hmm >>    dummy >> score1 >> dummy >> dummy >> score2;
  ASSERT (score1 > 0);
  ASSERT (score2 > 0)
  find (batch. hmm2fam, hmm, fam);
}



HmmAlignment::HmmAlignment (const HmmAlignment &other)
: sseqid (other. sseqid)
, score1 (other. score1)
, score2 (other. score2)
, fam    (other. fam)
, blastAl (new BlastAlignment (* other. blastAl. get ()))
{}



HmmAlignment HmmAlignment::operator= (const HmmAlignment &other)
{
  sseqid = other. sseqid;
  score1 = other. score1;
  score2 = other. score2;
  fam    = other. fam;
  blastAl. reset (new BlastAlignment (* other. blastAl. get ()));
  return *this;
}




// Domain

HmmAlignment::Domain::Domain (const string &line,
                              Batch &batch)
{
  static Istringstream iss;
  iss. reset (line);
  string target_name, accession, query_name, query_accession;
  size_t n, of, env_from, env_to;
  double eValue, full_score, full_bias, cValue, i_eValue, domain_bias, accuracy;
  iss >> target_name >> accession >> seqLen 
      >> query_name >> query_accession >> hmmLen 
      >> eValue >> full_score >> full_bias 
      >> n >> of >> cValue >> i_eValue >> score >> domain_bias 
      >> hmmStart >> hmmStop 
      >> seqStart >> seqStop 
      >> env_from >> env_to
      >> accuracy;
  ASSERT (accession == "-");
  ASSERT (hmmStart);
  ASSERT (seqStart);
  ASSERT (env_from);
  hmmStart--;
  seqStart--;
  env_from--;
  ASSERT (hmmStart < hmmStop);
  ASSERT (seqStart < seqStop);
  ASSERT (hmmStop <= hmmLen);
  ASSERT (seqStop <= seqLen);
  ASSERT (full_score > 0);
  ASSERT (n >= 1);
  ASSERT (n <= of);
  ASSERT (score > 0);

  const Fam* fam = batch. hmm2fam [query_accession];
  if (! fam)
    return;
  const HmmAlignment::Pair p (target_name, fam->id);
  const HmmAlignment::Domain domain_old (batch. domains [p]);
  if (domain_old. score > score)
    return;
  batch. domains [p] = *this;
}




// ThisApplication

struct ThisApplication : Application
{
  ThisApplication ()
    : Application ("Report AMR proteins")
    {
      // Input
      addKey ("fam", "Table FAM");
      const string blastFormat ("qseqid sseqid length nident qstart qend qlen sstart send slen sseq qseq. qseqid format: gi|Protein accession|fusion part|# fusions|FAM.id|FAM.class|Product name");
      addKey ("blastp", "blastp output in the format: " + blastFormat);  
      addKey ("blastx", "blastx output in the format: " + blastFormat);  
      addKey ("gff", ".gff assembly file");
      addKey ("gff_match", ".gff-FASTA matching file for \"locus_tag\": \"<FASTA id> <locus_tag>\"");
      addFlag ("bed", "Browser Extensible Data format of the <gff> file");
      addKey ("dna_len", "File with lines: <dna id> <dna length>");
      addKey ("hmmdom", "HMM domain alignments");
      addKey ("hmmsearch", "Output of hmmsearch");
      addKey ("organism", "Taxonomy group for point mutations");
      addKey ("point_mut", "Point mutation table");
      addKey ("point_mut_all", "File to report all target positions of reference point mutations");
      addKey ("ident_min", "Min. identity to the reference protein (0..1)", "0.9");
      addKey ("complete_cover_min", "Min. coverage of the reference protein (0..1) for a complete hit, otherwise it is partial", "0.9");
      addKey ("partial_cover_min", "Min. coverage of the reference protein (0..1) for a partial hit", "0.5");
      addFlag ("skip_hmm_check", "Skip checking HMM for a BLAST hit");
      // Output
      addKey ("out", "Identifiers of the reported input proteins");
      addFlag ("print_fam", "Print the FAM.id instead of gene symbol"); 
      addFlag ("pseudo", "Indicate pseudo-genes in the protein name as " + strQuote (stopCodonS) + " or " + strQuote (frameShiftS)); 
      addFlag ("force_cds_report", "Report contig/start/stop/strand even if this information does not exist");
      addFlag ("non_reportable", "Report non-reportable families");
      // Testing
      addFlag ("nosame", "Exclude the same reference ptotein from the BLAST output (for testing)"); 
      addFlag ("noblast", "Exclude the BLAST output (for testing)"); 
    #ifdef SVN_REV
      version = SVN_REV;
    #endif
    }



  void body () const final
  {
    const string famFName            = getArg ("fam");
    const string blastpFName         = getArg ("blastp");
    const string blastxFName         = getArg ("blastx");
    const string gffFName            = getArg ("gff");
    const string gffMatchFName       = getArg ("gff_match");
    const bool   bedP                = getFlag ("bed");
    const string dnaLenFName         = getArg ("dna_len");
    const string hmmDom              = getArg ("hmmdom");
    const string hmmsearch           = getArg ("hmmsearch");  
    const string organism            = getArg ("organism");  
    const string point_mut           = getArg ("point_mut");  
    const string point_mut_all_FName = getArg ("point_mut_all");
    const double ident_min           = str2<double> (getArg ("ident_min"));  
                 complete_cover_min  = str2<double> (getArg ("complete_cover_min"));  
    const double partial_cover_min   = str2<double> (getArg ("partial_cover_min")); 
    const bool   skip_hmm_check      = getFlag ("skip_hmm_check"); 
    const string outFName            = getArg ("out");
                 print_fam           = getFlag ("print_fam");
                 reportPseudo        = getFlag ("pseudo");
    const bool   force_cds_report    = getFlag ("force_cds_report");
    const bool   non_reportable      = getFlag ("non_reportable");
    const bool   nosame              = getFlag ("nosame");
    const bool   noblast             = getFlag ("noblast");
    
    ASSERT (hmmsearch. empty () == hmmDom. empty ());
    IMPLY (! outFName. empty (), ! blastpFName. empty ());
    IMPLY (! gffFName. empty (), ! blastpFName. empty ());
    if (! blastpFName. empty () && ! blastxFName. empty () && gffFName. empty ())
    	throw runtime_error ("If BLASTP and BLASTX files are present then a GFF file must be present");
    
    if (! (ident_min >= 0 && ident_min <= 1))
    	throw runtime_error ("ident_min must be between 0 and 1");
    if (! (complete_cover_min >= 0 && complete_cover_min <= 1))
    	throw runtime_error ("complete_cover_min must be between 0 and 1");
    if (! (partial_cover_min >= 0 && partial_cover_min <= 1))
    	throw runtime_error ("partial_cover_min must be between 0 and 1");
    if (partial_cover_min > complete_cover_min)
    	throw runtime_error ("partial_cover_min must be less than or equal to complete_cover_min");

    globalCompleteBR = BlastRule (ident_min, complete_cover_min, complete_cover_min);
    globalPartialBR  = BlastRule (ident_min, complete_cover_min, partial_cover_min);
    
    
  //combiner = ! blastpFName. empty () && ! blastxFName. empty ();
    
    cdsExist =    force_cds_report
               || ! blastxFName. empty ()
               || ! gffFName. empty ();


    if (! point_mut_all_FName. empty ())
      point_mut_all. reset (new OFStream (point_mut_all_FName));
      

    Batch batch (famFName, organism, point_mut, non_reportable);  
  
  
    // Fusion proteins, see PD-283 ??
    
    
    // Input 

    // batch.blastAls
  	// BlastAlignment::good()
    if (! noblast)
    {
      if (! blastpFName. empty ())
      {
        LineInput f (blastpFName);
    	  while (f. nextLine ())
    	  {
    	    { 
    	      Unverbose unv;
    	      if (verbose ())
    	        cout << f. line << endl;  
    	    }
    	    BlastAlignment al (f. line, true);
    	    al. qc ();  
    	    if (nosame && toString (al. gi) == al. targetName)
    	      continue;
    	    if (al. good ())
    	      batch. blastAls << move (al);
    	    else
      	    { ASSERT (! al. refExactlyMatched ());
      	      if (false)
      	      {
      	        al. saveText (cout);
      	        cout << al. targetName << endl;
      	        cout << al. pIdentity () << ' ' << al. refCoverage () << ' ' << al. targetCoverage () << endl;
      	        ERROR;
      	      }
      	    }
    	  }
    	}

      if (! blastxFName. empty ())
      {
        LineInput f (blastxFName);
    	  while (f. nextLine ())
    	  {
    	    { 
    	      Unverbose unv;
    	      if (verbose ())
    	        cout << f. line << endl;  
    	    }
    	    BlastAlignment al (f. line, false);
    	    al. qc ();  
    	    if (nosame && toString (al. gi) == al. targetName)
    	      continue;
    	    if (al. good ())
    	      batch. blastAls << move (al);
    	    else
      	    { ASSERT (! al. refExactlyMatched ()); }
    	  }
    	}
    }
  	if (verbose ())
  	  cout << "# Good Blasts: " << batch. blastAls. size () << endl;
  	
  
    // batch.domains
    if (! hmmDom. empty ())
    {
    	batch. hmmExist = true;
      LineInput f (hmmDom);
  	  while (f. nextLine ())
  	  {
  	    trim (f. line);
  	    if (   f. line. empty () 
  	        || f. line [0] == '#'
  	       )
  	      continue;
  	    const HmmAlignment::Domain domain (f. line, batch);
  	  }
    }


    // batch.hmmAls 
  	// HmmAlignment::good()
  	if (! hmmsearch. empty ())  // redundant file ??
  	{
      LineInput f (hmmsearch);
  	  while (f. nextLine ())
  	  {
  	    if (verbose ())
  	      cout << f. line << endl;  
  	    if (f. line. empty () || f. line [0] == '#')
  	      continue;
  	    HmmAlignment hmmAl (f. line, batch);
  	    if (! hmmAl. good ())
  	    	continue;
  	    auto al = new BlastAlignment (hmmAl);
	  	  hmmAl. blastAl. reset (al);
	  	  if (verbose ())
	  	    cout << al->targetName << " " << al->gene << endl;  
	  	  const HmmAlignment::Domain domain = batch. domains [HmmAlignment::Pair (al->targetName, al->gene)];
	  	  if (! domain. hmmLen)  
	  	    continue;  // domain does not exist
	  	/*al->refLen      = domain. hmmLen;
	  	  al->refStart    = domain. hmmStart;
	  	  al->refStop     = domain. hmmStop; */
	  	  al->targetLen   = domain. seqLen;
	  	  al->targetStart = domain. seqStart;
	  	  al->targetStop  = domain. seqStop;
	  	  al->setTargetAlign ();
	  	  ASSERT (! al->refExactlyMatched ());
	  	  al->qc ();
	      batch. hmmAls << move (hmmAl);
  	  }
  	}
  	if (verbose ())
  	  cout << "# Good HMMs: " << batch. hmmAls. size () << endl;
  
  
    // For Batch::report()
    if (! gffFName. empty ())
    {
    	unique_ptr<const Annot> annot;
    	if (bedP)
    	{
	    	Annot::Bed bed;
		    annot. reset (new Annot (bed, gffFName));
		  }
    	else
    	{
	    	Annot::Gff gff;
		    annot. reset (new Annot (gff, gffFName, ! gffMatchFName. empty ()));
		  }
		  ASSERT (annot. get ());
	    map<string/*seqid*/,string/*locusTag*/> seqId2locusTag;
	    if (! gffMatchFName. empty ())
	    {
	      LineInput f (gffMatchFName);
   	  	Istringstream iss;
  	  	string seqId, locusTag;
	  	  while (f. nextLine ())
	  	  {
    	  	iss. reset (f. line);
	  	  	iss >> seqId >> locusTag;
	  	  	ASSERT (! locusTag. empty ());
	  	  	seqId2locusTag [seqId] = locusTag;
	  	  }
	  	  if (seqId2locusTag. empty ())
	  	  	throw runtime_error ("File " + gffMatchFName + " is empty");
	    }
	    {
	      // Locus::contigLen
  	    map<string,size_t> contig2len;
  	    if (! dnaLenFName. empty ())
  	    {
  	      LineInput f (dnaLenFName);
  	      string contig;
  	      size_t len;
  	      Istringstream iss;
  	      while (f. nextLine ())
  	      {
  	        iss. reset (f. line);
  	        len = 0;
  	        iss >> contig >> len;
  	        ASSERT (len);
  	        contig2len [contig] = len;
  	      }
  	      for (const auto& it : annot->prot2cdss)
  	        for (const Locus& locus : it. second)
  	          if (! locus. contigLen)
  	            var_cast (locus). contigLen = contig2len [locus.contig];
  	    }
  	  }
    	for (auto& al : batch. blastAls)
    	  if (al. targetProt)
    	  	al. setCdss (seqId2locusTag, * annot. get ());
	    for (auto& hmmAl : batch. hmmAls)
        var_cast (hmmAl. blastAl. get ()) -> setCdss (seqId2locusTag, * annot. get ());;
    }
    
    
    // Output
    batch. process (skip_hmm_check);    
    batch. report (cout);
    if (! outFName. empty ())
    {
	    OFStream ofs (outFName);
      batch. printTargetIds (ofs);
    }
  }
};



}  // namespace



int main (int argc, 
          const char* argv[])
{
  ThisApplication app;
  return app. run (argc, argv);  
}



