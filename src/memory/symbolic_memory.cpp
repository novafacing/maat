#include "maat/memory.hpp"
#include "maat/stats.hpp"
#include "maat/varcontext.hpp"
#include <algorithm>
#include <list>
#include <memory>

namespace maat
{

IntervalTree::IntervalTree(ucst_t min, ucst_t max):left(nullptr), right(nullptr)
{
    center = min + (max-min)/2;
}

void IntervalTree::add_interval(ucst_t min, ucst_t max, int write_count)
{
    std::list<SimpleInterval>::iterator idx;
    if( min <= center && max >= center )
    {
        // Interval contains 'center'
        // Check if the interval is entirely contained in another previous interval 
        if( (idx = std::find_if(
                match_min.begin(),
                match_min.end(), 
                [&min, &max](SimpleInterval& si){return si.min <= min && si.max >= max;}
            )) != match_min.end() 
        )
        {
            return; // Don't add it
        } 

        // Add the interval
        auto index_min = std::lower_bound(
            match_min.begin(),
            match_min.end(),
            min, 
            [](SimpleInterval& i, ucst_t val){ return i.min < val;}
        );
        match_min.insert(index_min, SimpleInterval(min, max, write_count));
        auto index_max = std::lower_bound(
            match_max.begin(),
            match_max.end(),
            max, 
            [](SimpleInterval& i, ucst_t val){ return i.max > val;}
        );
        match_max.insert(index_max, SimpleInterval(min, max, write_count));
    }
    else if (max < center)
    {
        // Add left
        if (left == nullptr)
        {
            left = std::make_unique<IntervalTree>(min, center);
        }
        left->add_interval(min, max, write_count);
    }
    else
    {
        // Add right
        if( right == nullptr )
        {
            right = std::make_unique<IntervalTree>(center, max);
        }
        right->add_interval(min, max, write_count);
    }
}

void IntervalTree::restore(int write_count)
{
    // Remove intervals in current node
    match_min.remove_if( [&write_count](SimpleInterval& i){return i.write_count > write_count;});
    match_max.remove_if( [&write_count](SimpleInterval& i){return i.write_count > write_count;});

    // Remove intervals in following nodes
    if( left )
        left->restore(write_count);
    if( right )
        right->restore(write_count);
}

bool IntervalTree::contains_addr(ucst_t val, unsigned int max_count)
{
    return contains_interval(val, val, max_count);
}

// Check if contains any address between min and max
bool IntervalTree::contains_interval(ucst_t min, ucst_t max, unsigned int max_count)
{
    if( !match_min.empty() )
    {
        // Check intervals here
        if( match_min.front().min <= max && match_max.front().max >= min )
        {
            for( SimpleInterval& si : match_min )
            {
                if( si.write_count > max_count )
                    continue; // Interval was written AFTER the value we want to check
                              // so didn't exist at the time
                if( si.min > max )
                {
                    break; // intervals start higher than max :(
                }else if( si.max >= min )
                {
                    return true; // interval is between min and max
                }
            }
        }
    }
    // Didn't find matching interval in this node
    // Try left ?
    if( left && (min < center) )
    {
        if( left->contains_interval(min, max, max_count) )
        {
            return true;
        }
    }
    // Try right ? 
    if( right && (max > center) )
    {
        if( right->contains_interval(min, max, max_count) )
        {
            return true;
        }
    }
    return false;
}

IntervalTree::~IntervalTree()
{
    // Now that left and right are unique_ptr they will be
    // deleted automatically
}

uid_t IntervalTree::class_uid() const
{
    return serial::ClassId::INTERVAL_TREE;
}

void IntervalTree::dump(Serializer& s) const
{
    s << bits(center) << left.get() << right.get() << match_min << match_max;
}

void IntervalTree::load(Deserializer& d)
{
    IntervalTree *t1, *t2;
    d >> bits(center) >> t1 >> t2 >> match_min >> match_max;
    left = std::unique_ptr<IntervalTree>(t1);
    right = std::unique_ptr<IntervalTree>(t2);
}


SymbolicMemEngine::SymbolicMemEngine(size_t arch_bits, std::shared_ptr<VarContext> varctx):
    _varctx(varctx),
    write_intervals(0, maat::cst_mask(arch_bits)),
    write_count(0),
    symptr_force_aligned(false)
{}

// min, max : the refined value set of 'addr'
void SymbolicMemEngine::symbolic_ptr_write(const Expr& addr, const Value& val, addr_t min, addr_t max)
{
    write_count++;
    ValueSet refined_vs(addr->size);
    refined_vs.set(min, max, addr->value_set().stride);
    // If max is close to MAX_INT, arrondir à MAX_INT
    if( cst_mask(val.size()) - max + 1 < (val.size()/8))
    {
        max = cst_mask(val.size());
    }
    // Record the write in the interval tree
    write_intervals.add_interval(min, max - 1 + (val.size()/8), write_count);
    // Add write to the list of writes
    writes.push_back(SymbolicMemWrite(addr, val, refined_vs));

    // Record the write in statistics
    MaatStats::instance().add_symptr_write(refined_vs.range());
}

void SymbolicMemEngine::concrete_ptr_write(Expr addr, const Value& val)
{
    if( write_intervals.contains_interval(addr->as_uint(*_varctx), addr->as_uint(*_varctx) -1 + (val.size()/8)) )
    {
        // Add writes
        writes.push_back(SymbolicMemWrite(addr, val, addr->value_set()));
        write_count++;
    }
}

void SymbolicMemEngine::concrete_ptr_write_buffer(Expr addr, uint8_t* src, int nb_bytes, size_t arch_bits)
{
    addr_t concrete_addr = addr->as_uint(*_varctx);
    for( int i = 0; i < nb_bytes; i++ )
    {
        if( write_intervals.contains_addr(concrete_addr+i))
        {
            // Add write
            writes.push_back(SymbolicMemWrite(concrete_addr, arch_bits, exprcst(8, src[i])));
            write_count++;
        }
    }
}

bool SymbolicMemEngine::contains_symbolic_write(addr_t start, addr_t end)
{
    return write_intervals.contains_interval(start, end);
}


// Return the expression result of the fact that 'expr' is
// written over 'prev' starting from byte 'index'
// WARNING!!!! This function assumes little endian
Expr _mem_expr_overwrite(Expr prev, Expr expr, int index)
{
    if( index < 0 ) // Overwrite before first byte
    {
        if( (index*8) + expr->size == prev->size)
        {
            // Write was bigger than prev and finishes exactly 
            // at the end of prev. We get the HSB of expr
            return extract(expr, expr->size-1, expr->size - prev->size);
        }
        else if((index*8) + expr->size > prev->size)
        {
            // Write still finishes after prev
            return extract(expr, -1*(index*8) + prev->size - 1, -1*(index*8));
        }
        else
        {
            // Write finishes before the end of prev (keep the LSB of both)
            return concat(  extract(prev, prev->size-1, expr->size + (index*8)), 
                        extract(expr, expr->size-1, -1*(index*8)));
        }
    }
    else if( index == 0 ) // Overwrite from first byte
    {
        if( prev->size == expr->size ){
            return expr;
        }
        else if( prev->size < expr->size )
        {
            return extract(expr, prev->size-1, 0);
        }
        else
        {
            return concat(extract(prev, prev->size-1,  expr->size), expr);
        }
    }
    // Index > 0
    else if( prev->size <= expr->size + (index*8)) // Overwrite to last byte
    {
        return concat(extract(expr, (prev->size - (index*8))-1, 0),
                      extract(prev, (index*8)-1, 0));
    }
    else // Overwrite in the middle
    {
        return concat(extract(prev, prev->size-1, expr->size+(index*8)),
                concat(expr, extract(prev, (index*8)-1, 0))
                );
    }
}

Expr SymbolicMemEngine::concrete_ptr_read(Expr addr, int nb_bytes, Expr base_expr)
{
    int i = 0;
    addr_t addr_min = addr->as_uint(*_varctx);
    Expr res = base_expr;
    Expr tmp_res;

    for( int count = 0; count < write_count; count++ )
    {
        SymbolicMemWrite& write = writes[count];
        if( write.refined_value_set.is_cst())
        {
            // Only update value if concrete write falls into the range of the read
            if(
                write.refined_value_set.min > addr_min - write.value.size()/8 and
                write.refined_value_set.min < addr_min + nb_bytes
            )
            {
                // Concrete ptr write recorded, we are sure
                res = _mem_expr_overwrite(res, write.value.as_expr(), write.refined_value_set.min - addr_min);
            }
        }
        else
        {
            // Symbolic ptr write -> set all possibilities
            // Change the possible values depending on possible overlaps
            i = 1 - write.value.size()/8; // Byte offset counter (we start a negative because a value
                                         // written before the read can still overwrite the read
            tmp_res = res;
            while (i < nb_bytes)
            {
                // Check if aligned (if option is enabled)
                if( symptr_force_aligned and (i%(write.value.size()/8) != 0))
                {
                    i++;
                    continue;
                }
                // Check if offset is compatible with stride
                if( !write.refined_value_set.contains(addr_min+i))
                {
                    i++;
                    continue;
                }
                // Only update if the interval of the write allows it to be written starting at
                // byte 'i'
                else
                {
                    res = ITE(write.addr, ITECond::EQ, addr+i, _mem_expr_overwrite(tmp_res, write.value.as_expr(), i), res);
                }
                i++;
            }
        }
    }
    return res;
}

Expr SymbolicMemEngine::symbolic_ptr_read(Expr& addr, ValueSet& addr_value_set, int nb_bytes, Expr base_expr)
{
    int i;
    int step;
    addr_t addr_min = addr_value_set.min;
    addr_t addr_max = addr_value_set.max;
    Expr res = base_expr;
    Expr tmp_res;

    for (int count = 0; count < write_count; count++)
    {
        SymbolicMemWrite& write = writes[count];
        i = 1 - write.value.size()/8;
        tmp_res = res;
        if( write.value.size()/8 <= nb_bytes )
        {
            step = write.value.size()/8;
        }
        else
        {
            step = nb_bytes;
        }
        while (i < nb_bytes)
        {
            if (symptr_force_aligned && (i%step != 0))
            {
                i++;
                continue;
            }

            // Only update if the write and read symbolic ranges overlap. Here we might add some cases
            // that are in practice impossible, because the address 'addr+i' might not be writable
            // by the write even though the absolute value_sets overlap, but the impossible cases
            // added here should not be too numerous and be pruned by the SMT solver later on...
            if(
                write.refined_value_set.min <= addr_max+i
                and write.refined_value_set.max >= addr_min+i
            )
            {
                res = ITE(write.addr, ITECond::EQ, addr+i, _mem_expr_overwrite(tmp_res, write.value.as_expr(), i), res);
            }
            i++;
        }
    }
    return res;
}


symbolic_mem_snapshot_t SymbolicMemEngine::take_snapshot()
{
    return write_count;
}

void SymbolicMemEngine::restore_snapshot(symbolic_mem_snapshot_t id)
{
    if( id > write_count )
    {
        throw runtime_exception("SymbolicMemEngine::restore_snapshot(): got snapshot id higher than current write_count!");
    }
    write_count = id;
    write_intervals.restore(write_count); // Restore interval tree
    writes.erase(writes.begin() + id, writes.end()); // Remove writes history
}


uid_t SymbolicMemEngine::class_uid() const
{
    return serial::ClassId::SYMBOLIC_MEM_ENGINE;
}

void SymbolicMemEngine::dump(Serializer& s) const
{
    s << bits(write_count) << writes << write_intervals << _varctx << bits(symptr_force_aligned);
}

void SymbolicMemEngine::load(Deserializer& d)
{
    d >> bits(write_count) >> writes >> write_intervals >> _varctx >> bits(symptr_force_aligned);
}

} // namespace maat